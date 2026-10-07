#pragma once

#include "dbsp_plan_ir.hpp"

namespace dbsp_native {
// Incremental GROUP BY aggregation over bound expressions: retracts the old
// group row, applies weighted deltas to per-group accumulators, emits the new
// group row; the downstream sink integrates. Output row layout matches
// LogicalAggregate: group values first, then aggregate values.
//
// Global aggregates (no GROUP BY) always have exactly one output row, even
// for an empty input (COUNT=0, SUM/AVG/MIN/MAX=NULL) — the row is emitted on
// the first circuit step and retracted/re-emitted on every change.
class PlanAggregateNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  struct AggInstance {
    PlanAggSpec::Fn fn;
    int arg_idx = -1;    // index into the batch evaluator; -1 for COUNT_STAR
    int filter_idx = -1; // FILTER predicate slot in the batch evaluator
    bool distinct = false;
    bool integer_arg;
    bool decimal_arg = false;
    uint8_t decimal_scale = 0;
    duckdb::LogicalType return_type;
    // Order-sensitive aggregates: batch slots of the ORDER BY keys +
    // their direction/null placement; STRING_AGG separator
    std::vector<int> order_idxs;
    std::vector<std::pair<bool, bool>> order_dirs; // (ascending, nulls_first)
    std::string separator;
    double quantile = 0.5;
  };

  // exprs layout in the shared batch evaluator: group keys first, then
  // aggregate arguments (H4: keys/args evaluate per 2048-row chunk instead
  // of per row through a 1-row executor)
  PlanAggregateNode(dbsp::NodeId id, InputFn input_fn,
                    std::shared_ptr<PlanKeepAlive> keep_alive,
                    std::vector<const duckdb::Expression *> group_exprs,
                    std::vector<const duckdb::Expression *> arg_exprs,
                    std::vector<AggInstance> aggs,
                    duckdb::vector<duckdb::LogicalType> input_types,
                    std::string name = "plan_aggregate")
      : dbsp::Node(id, std::move(name)), input_fn_(std::move(input_fn)),
        num_groups_(group_exprs.size()), aggs_(std::move(aggs)) {
    for (const auto *e : arg_exprs) {
      group_exprs.push_back(e);
    }
    if (!group_exprs.empty()) {
      eval_ = std::make_unique<BatchEvaluator>(
          std::move(keep_alive), std::move(group_exprs),
          std::move(input_types));
    }
  }

  void step() override {
    output_.clear();
    has_output_ = false;
    const DuckDBZSet &changes = input_fn_();
    bool global = num_groups_ == 0;

    if (changes.empty()) {
      if (global && !global_emitted_) {
        output_.insert(result_row(DuckDBRow{}, states_[DuckDBRow{}]), 1);
        global_emitted_ = true;
        has_output_ = true;
      }
      return;
    }

    // Batch-evaluate group keys and aggregate args, then bucket the
    // pre-evaluated (args, weight) pairs by key
    using Contribution = std::pair<std::vector<duckdb::Value>, int64_t>;
    std::unordered_map<DuckDBRow, std::vector<Contribution>, DuckDBRowHash>
        buckets;

    std::vector<const DuckDBRow *> rows;
    std::vector<int64_t> weights;
    rows.reserve(BatchEvaluator::kBatch);
    weights.reserve(BatchEvaluator::kBatch);
    const size_t num_args = eval_ ? eval_->expr_count() - num_groups_ : 0;

    auto flush = [&]() {
      if (rows.empty()) {
        return;
      }
      const duckdb::idx_t n = rows.size();
      std::vector<DuckDBRow> keys(n);
      if (eval_) {
        eval_->fill(rows.data(), n);
        std::vector<std::vector<duckdb::Value>> key_vals(n);
        for (auto &kv : key_vals) {
          kv.reserve(num_groups_);
        }
        for (size_t g = 0; g < num_groups_; g++) {
          duckdb::Vector &v = eval_->execute(g);
          const auto &type = eval_->return_type(g);
          for (duckdb::idx_t i = 0; i < n; i++) {
            key_vals[i].push_back(BatchEvaluator::read_result(v, type, i));
          }
        }
        // DP3a: fold the executed key vectors into per-row hashes so the
        // bucket map lookups below never pay the per-Value lazy hash
        std::vector<size_t> key_hashes(n, 0);
        {
          duckdb::Vector hash_scratch(duckdb::LogicalType::HASH);
          for (size_t g = 0; g < num_groups_; g++) {
            fold_vector_hashes(eval_->result_vector(g), n, hash_scratch,
                               key_hashes);
          }
        }
        for (duckdb::idx_t i = 0; i < n; i++) {
          keys[i].columns.assign(std::move(key_vals[i]));
          keys[i].columns.set_hash(key_hashes[i]);
        }
        std::vector<std::vector<duckdb::Value>> args(n);
        for (size_t a = 0; a < num_args; a++) {
          const size_t e = num_groups_ + a;
          duckdb::Vector &v = eval_->execute(e);
          const auto &type = eval_->return_type(e);
          for (duckdb::idx_t i = 0; i < n; i++) {
            args[i].push_back(BatchEvaluator::read_result(v, type, i));
          }
        }
        for (duckdb::idx_t i = 0; i < n; i++) {
          buckets[std::move(keys[i])].emplace_back(std::move(args[i]),
                                                   weights[i]);
        }
      } else {
        for (duckdb::idx_t i = 0; i < n; i++) {
          buckets[DuckDBRow{}].emplace_back(std::vector<duckdb::Value>{},
                                            weights[i]);
        }
      }
      rows.clear();
      weights.clear();
    };
    for (const auto &[row, weight] : changes) {
      rows.push_back(&row);
      weights.push_back(weight);
      if (rows.size() == BatchEvaluator::kBatch) {
        flush();
      }
    }
    flush();

    for (auto &[key, contributions] : buckets) {
      auto &state = states_[key];
      bool had_row = global ? global_emitted_ : state.row_weight > 0;
      if (had_row) {
        output_.insert(result_row(key, state), -1);
      }
      for (const auto &[args, weight] : contributions) {
        apply(state, args, weight);
      }
      if (global || state.row_weight > 0) {
        output_.insert(result_row(key, state), 1);
      }
      if (!global && state.row_weight <= 0) {
        states_.erase(key);
      }
      if (global) {
        global_emitted_ = true;
      }
    }
    has_output_ = !output_.empty();
  }

  void reset() override {
    states_.clear();
    output_.clear();
    has_output_ = false;
    global_emitted_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

  void clear_output() override {
    output_.clear();
    has_output_ = false;
  }

  void account_state(StateBytes &out, StateAccounting &acct) const override {
    // Real per-group state, not a flat estimate — the old 96B/group figure
    // ignored every collecting container and underestimated MIN/MAX-heavy
    // groups 4-8x, which fed the spill-mode decisions.
    auto value_bytes = [](const duckdb::Value &v) -> size_t {
      size_t b = 72; // sizeof(duckdb::Value) + slop (kValueBytes)
      if (!v.IsNull() && v.type().id() == duckdb::LogicalTypeId::VARCHAR) {
        b += duckdb::StringValue::Get(v).size();
      }
      return b;
    };
    for (const auto &[key, gs] : states_) {
      out.other += acct.row_bytes(key) + 48; // hash node + GroupState header
      for (const auto &a : gs.aggs) {
        out.other += sizeof(AggState);
        if (!a.coll) {
          continue;
        }
        const auto &c = *a.coll;
        for (const auto &[v, cnt] : c.vcounts) {
          (void)cnt;
          out.other += value_bytes(v) + 48; // map node + count
        }
        for (const auto &[v, cnt] : c.dvals) {
          (void)cnt;
          out.other += value_bytes(v) + 48;
        }
        for (const auto &[v, cnt] : c.mode_counts) {
          (void)cnt;
          out.other += value_bytes(v) + 48;
        }
        for (const auto &[keys, v] : c.ordered) {
          out.other += value_bytes(v) + 48;
          for (const auto &k : keys) {
            out.other += value_bytes(k);
          }
        }
      }
    }
    out.other += acct.zset_bytes(output_);
  }

private:
  // F8 split: the POD scalars every aggregate uses stay inline (~48B);
  // the collecting containers — which only MIN/MAX/quantile-family,
  // DISTINCT, ordered (STRING_AGG/ARRAY_AGG) and MODE aggregates touch —
  // live behind one pointer allocated on first use. A SUM/COUNT-only
  // group previously paid ~144B of empty container headers per aggregate.
  struct AggState {
    int64_t count = 0; // non-NULL argument count (rows for COUNT(*))
    int64_t isum = 0;
    double dsum = 0;
    duckdb::hugeint_t hsum = 0; // DECIMAL SUM, unscaled

    struct Collecting {
      // MIN/MAX/MEDIAN/QUANTILE/MAD/FIRST: value -> multiplicity. A
      // weight-w row is ONE map node with count w (was: w duplicate
      // multiset nodes).
      std::map<duckdb::Value, int64_t> vcounts;
      // DISTINCT: per-value weights; contributions fire on presence
      // transitions (0→>0 adds the value once, >0→0 retracts it)
      std::map<duckdb::Value, int64_t> dvals;
      // Order-sensitive aggregates: (order keys, value) kept sorted; the
      // whole aggregate re-renders from this on every group change
      std::vector<std::pair<std::vector<duckdb::Value>, duckdb::Value>>
          ordered;
      // MODE: per-value multiplicities
      std::map<duckdb::Value, int64_t> mode_counts;
      // N4: a group whose DISTINCT value count grows past the threshold
      // (spill mode) moves its values to a disk record log; renders
      // reload the group when it is touched. mode_counts/ordered entries
      // stay in RAM. (Threshold is on vcounts.size() — distinct values —
      // which IS the RAM footprint in the count-map layout.)
      std::unique_ptr<SpilledBaseline> spilled_values;
    };
    std::unique_ptr<Collecting> coll;

    Collecting &collecting() {
      if (!coll) {
        coll = std::make_unique<Collecting>();
      }
      return *coll;
    }
  };

  struct GroupState {
    int64_t row_weight = 0; // total weight of rows in the group
    std::vector<AggState> aggs;
  };

  void apply(GroupState &state, const std::vector<duckdb::Value> &args,
             int64_t weight) {
    state.row_weight += weight;
    state.aggs.resize(aggs_.size());
    for (size_t i = 0; i < aggs_.size(); i++) {
      auto &spec = aggs_[i];
      auto &s = state.aggs[i];
      if (spec.filter_idx >= 0) {
        // FILTER (WHERE p): rows failing p contribute nothing to THIS
        // aggregate. Applies symmetrically to inserts and deletes, so
        // incremental maintenance is unchanged.
        const duckdb::Value &f = args[static_cast<size_t>(spec.filter_idx)];
        if (f.IsNull() || !f.GetValue<bool>()) {
          continue;
        }
      }
      if (spec.fn == PlanAggSpec::Fn::COUNT_STAR) {
        s.count += weight;
        continue;
      }
      const duckdb::Value &v = args[static_cast<size_t>(spec.arg_idx)];
      if (spec.fn == PlanAggSpec::Fn::STRING_AGG ||
          spec.fn == PlanAggSpec::Fn::ARRAY_AGG) {
        // string_agg skips NULLs; array_agg keeps them (DuckDB semantics)
        if (v.IsNull() && spec.fn == PlanAggSpec::Fn::STRING_AGG) {
          continue;
        }
        std::vector<duckdb::Value> okeys;
        okeys.reserve(spec.order_idxs.size());
        for (int oi : spec.order_idxs) {
          okeys.push_back(args[static_cast<size_t>(oi)]);
        }
        std::pair<std::vector<duckdb::Value>, duckdb::Value> entry{
            std::move(okeys), v};
        auto cmp = [&spec, this](const decltype(entry) &a,
                                 const decltype(entry) &b) {
          return ordered_less(spec, a, b);
        };
        auto &ordered = s.collecting().ordered;
        if (weight > 0) {
          for (int64_t w = 0; w < weight; w++) {
            auto it =
                std::upper_bound(ordered.begin(), ordered.end(), entry, cmp);
            ordered.insert(it, entry);
          }
        } else {
          for (int64_t w = 0; w < -weight; w++) {
            auto range =
                std::equal_range(ordered.begin(), ordered.end(), entry, cmp);
            // Erase one exact match (order keys AND value equal)
            for (auto it = range.first; it != range.second; ++it) {
              if (it->second.IsNull() == entry.second.IsNull() &&
                  (it->second.IsNull() || !(it->second < entry.second) &&
                                              !(entry.second < it->second))) {
                ordered.erase(it);
                break;
              }
            }
          }
        }
        continue;
      }
      if (v.IsNull()) {
        continue; // SQL: NULL arguments are ignored
      }
      if (spec.distinct) {
        // Presence transition drives COUNT/SUM/AVG; MIN/MAX fall through
        // (duplicates never change an extreme)
        auto &dvals = s.collecting().dvals;
        int64_t &dw = dvals[v];
        const bool was = dw > 0;
        dw += weight;
        const bool is = dw > 0;
        if (dw == 0) {
          dvals.erase(v);
        }
        if (spec.fn != PlanAggSpec::Fn::MIN &&
            spec.fn != PlanAggSpec::Fn::MAX) {
          const int64_t d = (is ? 1 : 0) - (was ? 1 : 0);
          if (d == 0) {
            continue;
          }
          s.count += d;
          if (spec.fn == PlanAggSpec::Fn::SUM ||
              spec.fn == PlanAggSpec::Fn::AVG) {
            if (spec.decimal_arg) {
              duckdb::Value wide = v.DefaultCastAs(
                  duckdb::LogicalType::DECIMAL(38, spec.decimal_scale));
              s.hsum += wide.GetValueUnsafe<duckdb::hugeint_t>() *
                        duckdb::hugeint_t(d);
            } else if (spec.integer_arg) {
              s.isum += v.GetValue<int64_t>() * d;
            } else {
              s.dsum += v.GetValue<double>() * d;
            }
          }
          continue;
        }
      }
      s.count += weight;
      switch (spec.fn) {
      case PlanAggSpec::Fn::COUNT:
        break;
      case PlanAggSpec::Fn::SUM:
      case PlanAggSpec::Fn::AVG:
        if (spec.decimal_arg) {
          // Exact: sum the unscaled decimal representation in 128 bits
          duckdb::Value wide = v.DefaultCastAs(
              duckdb::LogicalType::DECIMAL(38, spec.decimal_scale));
          s.hsum += wide.GetValueUnsafe<duckdb::hugeint_t>() *
                    duckdb::hugeint_t(weight);
        } else if (spec.integer_arg) {
          s.isum += v.GetValue<int64_t>() * weight;
        } else {
          s.dsum += v.GetValue<double>() * weight;
        }
        break;
      case PlanAggSpec::Fn::MIN:
      case PlanAggSpec::Fn::MAX:
      case PlanAggSpec::Fn::FIRST:
      case PlanAggSpec::Fn::MEDIAN:
      case PlanAggSpec::Fn::QUANTILE_CONT:
      case PlanAggSpec::Fn::QUANTILE_DISC:
      case PlanAggSpec::Fn::MAD: {
        auto &coll = s.collecting();
        if (coll.spilled_values) {
          coll.spilled_values->apply_row({v}, weight);
          break;
        }
        int64_t &c = coll.vcounts[v];
        c += weight;
        if (c <= 0) {
          coll.vcounts.erase(v);
        }
        // N4: oversized group → move values to disk (spill mode). Renders
        // reload the group only when it is touched again. Threshold is on
        // DISTINCT values — the map node count IS the RAM footprint.
        if (g_spill_mode.load() && coll.vcounts.size() > 65536) {
          coll.spilled_values = std::make_unique<SpilledBaseline>(
              g_spill_dir + "/agg_" +
              std::to_string(g_spill_file_seq.fetch_add(1)) + ".dbspill");
          for (const auto &[val, cnt] : coll.vcounts) {
            coll.spilled_values->apply_row({val}, cnt);
          }
          coll.vcounts.clear();
        }
        break;
      }
      case PlanAggSpec::Fn::MODE: {
        auto &mode_counts = s.collecting().mode_counts;
        int64_t &c = mode_counts[v];
        c += weight;
        if (c <= 0) {
          mode_counts.erase(v);
        }
        break;
      }
      default:
        break;
      }
    }
  }

  // Sort order for order-sensitive aggregate entries: the declared ORDER
  // BY keys (direction + null placement per key), then the value itself
  // as a deterministic tiebreak. duckdb::Value::operator< cannot compare
  // NULLs, so NULL handling comes first at every step.
  static bool value_less(const duckdb::Value &a, const duckdb::Value &b,
                         bool ascending, bool nulls_first) {
    const bool an = a.IsNull(), bn = b.IsNull();
    if (an || bn) {
      if (an && bn) {
        return false;
      }
      return an ? nulls_first : !nulls_first;
    }
    return ascending ? a < b : b < a;
  }

  bool ordered_less(
      const AggInstance &spec,
      const std::pair<std::vector<duckdb::Value>, duckdb::Value> &a,
      const std::pair<std::vector<duckdb::Value>, duckdb::Value> &b) const {
    for (size_t k = 0; k < spec.order_dirs.size(); k++) {
      const auto &[asc, nf] = spec.order_dirs[k];
      if (value_less(a.first[k], b.first[k], asc, nf)) {
        return true;
      }
      if (value_less(b.first[k], a.first[k], asc, nf)) {
        return false;
      }
    }
    // Tiebreak on the value (ascending, NULLs last) for determinism
    return value_less(a.second, b.second, true, false);
  }

  // N4: value-count renders read through this — a spilled group reloads
  // into `tmp` (touched groups only; untouched groups never re-render)
  using ValueCounts = std::map<duckdb::Value, int64_t>;
  static const ValueCounts &values_of(const AggState &s, ValueCounts &tmp) {
    static const ValueCounts kEmpty;
    if (!s.coll) {
      return kEmpty;
    }
    if (!s.coll->spilled_values) {
      return s.coll->vcounts;
    }
    s.coll->spilled_values->scan(
        [&](const std::vector<duckdb::Value> &vals, int64_t w) {
          tmp[vals[0]] += w;
        });
    return tmp;
  }

  // Total multiplicity of a value-count map (quantile denominators).
  static size_t total_of(const ValueCounts &values) {
    size_t n = 0;
    for (const auto &[v, c] : values) {
      (void)v;
      n += static_cast<size_t>(c);
    }
    return n;
  }

  // Values at 0-based ranks `lo_idx` and `lo_idx+1` of the weighted sorted
  // sequence (hi unused when lo is the last element).
  static void ranks_of(const ValueCounts &values, size_t lo_idx,
                       duckdb::Value &lo, duckdb::Value &hi) {
    size_t cum = 0;
    bool have_lo = false;
    for (const auto &[v, c] : values) {
      const size_t end = cum + static_cast<size_t>(c);
      if (!have_lo && lo_idx < end) {
        lo = v;
        have_lo = true;
      }
      if (lo_idx + 1 < end) {
        hi = v;
        return;
      }
      cum = end;
    }
    if (have_lo) {
      hi = lo; // lo was the final rank
    }
  }

  duckdb::Value agg_value(const AggInstance &spec, const AggState &s) const {
    ValueCounts spill_tmp;
    const ValueCounts &values = values_of(s, spill_tmp);
    switch (spec.fn) {
    case PlanAggSpec::Fn::COUNT_STAR:
    case PlanAggSpec::Fn::COUNT:
      return duckdb::Value::BIGINT(s.count);
    case PlanAggSpec::Fn::SUM:
      if (s.count == 0) {
        return duckdb::Value(spec.return_type);
      }
      if (spec.decimal_arg) {
        return duckdb::Value::DECIMAL(s.hsum, 38, spec.decimal_scale)
            .DefaultCastAs(spec.return_type);
      }
      if (spec.integer_arg) {
        return duckdb::Value::Numeric(spec.return_type, s.isum);
      }
      return duckdb::Value(s.dsum).DefaultCastAs(spec.return_type);
    case PlanAggSpec::Fn::AVG: {
      if (s.count == 0) {
        return duckdb::Value(spec.return_type);
      }
      double sum = spec.integer_arg ? static_cast<double>(s.isum) : s.dsum;
      return duckdb::Value(sum / static_cast<double>(s.count))
          .DefaultCastAs(spec.return_type);
    }
    case PlanAggSpec::Fn::MIN:
    case PlanAggSpec::Fn::FIRST:
      return values.empty() ? duckdb::Value(spec.return_type)
                              : values.begin()->first;
    case PlanAggSpec::Fn::MAX:
      return values.empty() ? duckdb::Value(spec.return_type)
                              : values.rbegin()->first;
    case PlanAggSpec::Fn::STRING_AGG: {
      if (!s.coll || s.coll->ordered.empty()) {
        return duckdb::Value(spec.return_type);
      }
      std::string out;
      bool first = true;
      for (const auto &[keys, v] : s.coll->ordered) {
        if (!first) {
          out += spec.separator;
        }
        out += v.DefaultCastAs(duckdb::LogicalType::VARCHAR)
                   .GetValue<std::string>();
        first = false;
      }
      return duckdb::Value(out);
    }
    case PlanAggSpec::Fn::MEDIAN:
    case PlanAggSpec::Fn::QUANTILE_CONT: {
      if (values.empty()) {
        return duckdb::Value(spec.return_type);
      }
      // Interpolated quantile over the weighted sorted values: position
      // q*(n-1) between rank neighbors lo and hi
      const double q =
          spec.fn == PlanAggSpec::Fn::MEDIAN ? 0.5 : spec.quantile;
      const size_t n = total_of(values);
      const double pos = q * static_cast<double>(n - 1);
      const size_t lo_idx = static_cast<size_t>(pos);
      const double frac = pos - static_cast<double>(lo_idx);
      duckdb::Value lo, hi;
      ranks_of(values, lo_idx, lo, hi);
      if (frac == 0.0 || lo_idx + 1 >= n) {
        return lo.DefaultCastAs(spec.return_type);
      }
      const double lod = lo.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                             .GetValue<double>();
      const double hid = hi.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                             .GetValue<double>();
      return duckdb::Value(lod + frac * (hid - lod))
          .DefaultCastAs(spec.return_type);
    }
    case PlanAggSpec::Fn::QUANTILE_DISC: {
      if (values.empty()) {
        return duckdb::Value(spec.return_type);
      }
      // Discrete quantile: element at ceil(q*n)-1 (DuckDB semantics)
      const size_t n = total_of(values);
      size_t idx = static_cast<size_t>(
          std::ceil(spec.quantile * static_cast<double>(n)));
      idx = idx > 0 ? idx - 1 : 0;
      if (idx >= n) {
        idx = n - 1;
      }
      duckdb::Value lo, hi;
      ranks_of(values, idx, lo, hi);
      return lo.DefaultCastAs(spec.return_type);
    }
    case PlanAggSpec::Fn::MAD: {
      if (values.empty()) {
        return duckdb::Value(spec.return_type);
      }
      // Median absolute deviation: median(|x - median(x)|), both medians
      // interpolated (DuckDB semantics). Deviations come out sorted by
      // merging the two halves around the median.
      std::vector<double> vals;
      vals.reserve(total_of(values));
      for (const auto &[v, c] : values) {
        const double d = v.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                             .GetValue<double>();
        for (int64_t k = 0; k < c; k++) {
          vals.push_back(d);
        }
      }
      auto interp = [](const std::vector<double> &sorted) {
        const double pos = 0.5 * static_cast<double>(sorted.size() - 1);
        const size_t lo = static_cast<size_t>(pos);
        const double frac = pos - static_cast<double>(lo);
        if (frac == 0.0 || lo + 1 >= sorted.size()) {
          return sorted[lo];
        }
        return sorted[lo] + frac * (sorted[lo + 1] - sorted[lo]);
      };
      const double med = interp(vals);
      std::vector<double> devs;
      devs.reserve(vals.size());
      size_t below = 0;
      while (below < vals.size() && vals[below] <= med) {
        below++;
      }
      size_t l = below, r = below; // l walks down, r walks up
      while (devs.size() < vals.size()) {
        const double dl =
            l > 0 ? med - vals[l - 1] : std::numeric_limits<double>::max();
        const double dr = r < vals.size()
                              ? vals[r] - med
                              : std::numeric_limits<double>::max();
        if (dl <= dr) {
          devs.push_back(dl);
          l--;
        } else {
          devs.push_back(dr);
          r++;
        }
      }
      return duckdb::Value(interp(devs)).DefaultCastAs(spec.return_type);
    }
    case PlanAggSpec::Fn::MODE: {
      if (!s.coll || s.coll->mode_counts.empty()) {
        return duckdb::Value(spec.return_type);
      }
      // Highest multiplicity; ties break by smallest value (std::map is
      // value-ordered, first hit wins) — DuckDB's tie choice is
      // scan-order-dependent and unreproducible incrementally
      const duckdb::Value *best = nullptr;
      int64_t best_count = 0;
      for (const auto &[v, c] : s.coll->mode_counts) {
        if (c > best_count) {
          best = &v;
          best_count = c;
        }
      }
      return best->DefaultCastAs(spec.return_type);
    }
    case PlanAggSpec::Fn::ARRAY_AGG: {
      if (!s.coll || s.coll->ordered.empty()) {
        return duckdb::Value(spec.return_type);
      }
      duckdb::vector<duckdb::Value> vals;
      vals.reserve(s.coll->ordered.size());
      for (const auto &[keys, v] : s.coll->ordered) {
        vals.push_back(v);
      }
      return duckdb::Value::LIST(
          duckdb::ListType::GetChildType(spec.return_type),
          std::move(vals));
    }
    }
    return duckdb::Value(spec.return_type);
  }

  DuckDBRow result_row(const DuckDBRow &key, const GroupState &state) const {
    std::vector<duckdb::Value> vals;
    vals.reserve(key.columns.size() + aggs_.size());
    vals.insert(vals.end(), key.columns.begin(), key.columns.end());
    for (size_t i = 0; i < aggs_.size(); i++) {
      static const AggState kEmpty;
      const AggState &s = i < state.aggs.size() ? state.aggs[i] : kEmpty;
      vals.push_back(agg_value(aggs_[i], s));
    }
    DuckDBRow result;
    result.columns.assign(std::move(vals));
    return result;
  }

  InputFn input_fn_;
  size_t num_groups_;
  std::unique_ptr<BatchEvaluator> eval_;
  std::vector<AggInstance> aggs_;
  std::unordered_map<DuckDBRow, GroupState, DuckDBRowHash> states_;
  DuckDBZSet output_;
  bool has_output_ = false;
  bool global_emitted_ = false;

public:
  // Checkpointing (D3b; extended Task 2, cold/restore attack): the
  // count/sum/avg family keeps only scalars per group — serializable.
  // Plain (non-DISTINCT) MIN/MAX also serialize: the `values` multiset is
  // exactly the retraction state agg_value()/apply() read (see MIN/MAX in
  // both), so round-tripping it is enough to resume correctly, including a
  // deleted extreme retreating to the next-highest/lowest remaining value.
  // DISTINCT (any fn — dvals weight-tracking untouched here) and holistic/
  // ordered aggregates (FIRST, STRING_AGG, ARRAY_AGG, MEDIAN, QUANTILE_*,
  // MODE, MAD) stay UNSUPPORTED. A group whose values have spilled to disk
  // (N4, values.size() > 65536 under spill mode) also declines the whole
  // node — the spilled log isn't captured here, mirroring the join node's
  // exclusion of spilled equi-key indexes (7512fd4).
  StateKind state_kind() const override {
    for (const auto &a : aggs_) {
      const bool scalar_fn = a.fn == PlanAggSpec::Fn::COUNT_STAR ||
                             a.fn == PlanAggSpec::Fn::COUNT ||
                             a.fn == PlanAggSpec::Fn::SUM ||
                             a.fn == PlanAggSpec::Fn::AVG;
      const bool value_collecting_fn = a.fn == PlanAggSpec::Fn::MIN ||
                                       a.fn == PlanAggSpec::Fn::MAX;
      if ((!scalar_fn && !value_collecting_fn) || a.distinct) {
        return StateKind::UNSUPPORTED;
      }
    }
    for (const auto &[key, group] : states_) {
      for (const auto &a : group.aggs) {
        if (a.coll && a.coll->spilled_values) {
          return StateKind::UNSUPPORTED;
        }
      }
    }
    return StateKind::SERIALIZABLE;
  }

  void serialize_state(std::vector<uint8_t> &out) const override {
    BlobWriter w;
    w.u64(static_cast<uint64_t>(global_emitted_ ? 1 : 0));
    w.u64(aggs_.size());
    w.u64(states_.size());
    for (const auto &[key, group] : states_) {
      w.row(key.columns);
      w.i64(group.row_weight);
      w.u64(group.aggs.size());
      for (const auto &a : group.aggs) {
        w.i64(a.count);
        w.i64(a.isum);
        w.f64(a.dsum);
        w.i64(a.hsum.upper);
        w.u64(a.hsum.lower);
        // MIN/MAX retraction state: the full value sequence, duplicates
        // preserved, as one row so a deleted extreme retreats correctly
        // post-restore (empty for scalar-only aggs — cheap no-op row).
        // Counts are EXPANDED to unit values so the wire format matches
        // the pre-count-map layout — old and new checkpoints stay
        // mutually readable.
        std::vector<duckdb::Value> vals;
        if (a.coll) {
          for (const auto &[v, c] : a.coll->vcounts) {
            for (int64_t k = 0; k < c; k++) {
              vals.push_back(v);
            }
          }
        }
        w.row(vals);
      }
    }
    out = w.take();
  }

  bool restore_state(const uint8_t *data, size_t len) override {
    try {
      BlobReader r(data, len);
      global_emitted_ = r.u64() != 0;
      if (r.u64() != aggs_.size()) {
        return false; // definition changed since save
      }
      states_.clear();
      const uint64_t n_groups = r.u64();
      for (uint64_t g = 0; g < n_groups; g++) {
        DuckDBRow key = r.hashed_row();
        GroupState group;
        group.row_weight = r.i64();
        const uint64_t n_aggs = r.u64();
        group.aggs.resize(n_aggs);
        for (uint64_t i = 0; i < n_aggs; i++) {
          auto &a = group.aggs[i];
          a.count = r.i64();
          a.isum = r.i64();
          a.dsum = r.f64();
          a.hsum.upper = r.i64();
          a.hsum.lower = r.u64();
          auto vals = r.row();
          if (!vals.empty()) {
            auto &vcounts = a.collecting().vcounts;
            for (auto &v : vals) {
              vcounts[v] += 1;
            }
          }
        }
        states_.emplace(std::move(key), std::move(group));
      }
      return r.done();
    } catch (...) {
      return false;
    }
  }
};

} // namespace dbsp_native
