#pragma once

#include "dbsp_plan_join.hpp"

namespace dbsp_native {
// Incremental DISTINCT (B3): tracks integrated multiplicity per row; emits
// +1 when a row's count crosses 0 -> positive, -1 when it drops to 0.
class PlanDistinctNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  PlanDistinctNode(dbsp::NodeId id, InputFn input_fn,
                   std::string name = "plan_distinct")
      : dbsp::Node(id, std::move(name)), input_fn_(std::move(input_fn)) {}

  void step() override {
    output_.clear();
    has_output_ = false;
    const DuckDBZSet &changes = input_fn_();
    for (const auto &[row, w] : changes) {
      int64_t &count = counts_[row];
      int64_t old_count = count;
      count += w;
      if (old_count <= 0 && count > 0) {
        output_.insert(row, 1);
      } else if (old_count > 0 && count <= 0) {
        output_.insert(row, -1);
      }
      if (count == 0) {
        counts_.erase(row);
      }
    }
    has_output_ = !output_.empty();
  }

  void reset() override {
    counts_.clear();
    output_.clear();
    has_output_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

  void clear_output() override {
    output_.clear();
    has_output_ = false;
  }

  // Bounded-RAM Phase 3 (reattach): DISTINCT state is one weight per row —
  // trivially serializable. Without this, every view containing DISTINCT
  // declined checkpointing and cold-replayed at every load.
  StateKind state_kind() const override { return StateKind::SERIALIZABLE; }

  void serialize_state(std::vector<uint8_t> &out) const override {
    BlobWriter w;
    w.u64(counts_.size());
    for (const auto &[row, c] : counts_) {
      w.row(row.columns);
      w.i64(c);
    }
    out = w.take();
  }

  bool restore_state(const uint8_t *data, size_t len) override {
    try {
      BlobReader r(data, len);
      counts_.clear();
      const uint64_t n = r.u64();
      for (uint64_t i = 0; i < n; i++) {
        DuckDBRow row = r.hashed_row();
        counts_[std::move(row)] = r.i64();
      }
      return r.done();
    } catch (...) {
      return false;
    }
  }

  void account_state(StateBytes &out, StateAccounting &acct) const override {
    for (const auto &[row, c] : counts_) {
      (void)c;
      out.other += acct.row_bytes(row) + 40;
    }
    out.other += acct.zset_bytes(output_);
  }

private:
  InputFn input_fn_;
  std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> counts_;
  DuckDBZSet output_;
  bool has_output_ = false;
};

// Incremental set operation (B3). Tracks integrated per-input multiplicity
// for every row and emits the change in output multiplicity:
//   UNION ALL      Σ counts             (stateless in principle, but kept
//                                        uniform for simplicity)
//   UNION          Σ counts > 0 ? 1 : 0
//   INTERSECT ALL  min(counts)          (2 inputs)
//   INTERSECT      all counts > 0 ? 1:0 (2 inputs)
//   EXCEPT ALL     max(a - b, 0)        (2 inputs)
//   EXCEPT         a > 0 && b == 0 ?1:0 (2 inputs)
class PlanSetOpNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;
  using SetOp = PlanOpSpec::SetOp;

  PlanSetOpNode(dbsp::NodeId id, std::vector<InputFn> inputs, SetOp op,
                std::string name = "plan_setop")
      : dbsp::Node(id, std::move(name)), inputs_(std::move(inputs)), op_(op) {}

  void step() override {
    output_.clear();
    has_output_ = false;

    // Collect changed rows and apply deltas per input
    std::unordered_map<DuckDBRow, std::vector<int64_t>, DuckDBRowHash>
        old_counts;
    for (size_t i = 0; i < inputs_.size(); i++) {
      const DuckDBZSet &delta = inputs_[i]();
      for (const auto &[row, w] : delta) {
        auto &counts = counts_[row];
        counts.resize(inputs_.size(), 0);
        if (!old_counts.count(row)) {
          old_counts[row] = counts;
        }
        counts[i] += w;
      }
    }

    for (const auto &[row, old] : old_counts) {
      auto it = counts_.find(row);
      const std::vector<int64_t> &now = it->second;
      int64_t delta = multiplicity(now) - multiplicity(old);
      if (delta != 0) {
        output_.insert(row, delta);
      }
      bool all_zero = true;
      for (int64_t c : now) {
        if (c != 0) {
          all_zero = false;
          break;
        }
      }
      if (all_zero) {
        counts_.erase(it);
      }
    }
    has_output_ = !output_.empty();
  }

  void reset() override {
    counts_.clear();
    output_.clear();
    has_output_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

  void clear_output() override {
    output_.clear();
    has_output_ = false;
  }

  // Bounded-RAM Phase 3 (reattach): set-op state is per-row input counts —
  // serializable. Without this, every UNION view (all compiled u_li_
  // rollup unions) declined checkpointing and cold-replayed at every load,
  // cascading realizes through the whole DAG at reattach.
  StateKind state_kind() const override { return StateKind::SERIALIZABLE; }

  void serialize_state(std::vector<uint8_t> &out) const override {
    BlobWriter w;
    w.u64(counts_.size());
    for (const auto &[row, ws] : counts_) {
      w.row(row.columns);
      w.u64(ws.size());
      for (const int64_t c : ws) {
        w.i64(c);
      }
    }
    out = w.take();
  }

  bool restore_state(const uint8_t *data, size_t len) override {
    try {
      BlobReader r(data, len);
      counts_.clear();
      const uint64_t n = r.u64();
      for (uint64_t i = 0; i < n; i++) {
        DuckDBRow row = r.hashed_row();
        const uint64_t k = r.u64();
        std::vector<int64_t> ws(k);
        for (uint64_t j = 0; j < k; j++) {
          ws[j] = r.i64();
        }
        counts_[std::move(row)] = std::move(ws);
      }
      return r.done();
    } catch (...) {
      return false;
    }
  }

  void account_state(StateBytes &out, StateAccounting &acct) const override {
    for (const auto &[row, ws] : counts_) {
      out.other += acct.row_bytes(row) + 40 + ws.capacity() * sizeof(int64_t);
    }
    out.other += acct.zset_bytes(output_);
  }

private:
  int64_t multiplicity(const std::vector<int64_t> &counts) const {
    auto at = [&](size_t i) {
      return i < counts.size() ? std::max<int64_t>(counts[i], 0) : 0;
    };
    switch (op_) {
    case SetOp::UNION_ALL: {
      int64_t total = 0;
      for (size_t i = 0; i < counts.size(); i++) {
        total += at(i);
      }
      return total;
    }
    case SetOp::UNION: {
      for (size_t i = 0; i < counts.size(); i++) {
        if (at(i) > 0) {
          return 1;
        }
      }
      return 0;
    }
    case SetOp::INTERSECT_ALL:
      return std::min(at(0), at(1));
    case SetOp::INTERSECT:
      return at(0) > 0 && at(1) > 0 ? 1 : 0;
    case SetOp::EXCEPT_ALL:
      return std::max<int64_t>(at(0) - at(1), 0);
    case SetOp::EXCEPT:
      return at(0) > 0 && at(1) == 0 ? 1 : 0;
    }
    return 0;
  }

  std::vector<InputFn> inputs_;
  SetOp op_;
  std::unordered_map<DuckDBRow, std::vector<int64_t>, DuckDBRowHash> counts_;
  DuckDBZSet output_;
  bool has_output_ = false;
};

// Runs an existing NativeMaterializedView as a circuit node fed from any
// upstream node (not just a base table). Used for view types with proven
// incremental logic that isn't decomposed into fine-grained nodes yet —
// currently NativeWindowView. The wrapped view is constructed with
// kInputName as its source table; every circuit step feeds it the upstream
// delta and exposes the view's own delta as this node's output.
class EmbeddedViewNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;
  static constexpr const char *kInputName = "__plan_embedded_input__";

  EmbeddedViewNode(dbsp::NodeId id, InputFn input_fn,
                   std::unique_ptr<NativeMaterializedView> view)
      : dbsp::Node(id, view->name() + "_embedded"),
        input_fn_(std::move(input_fn)), view_(std::move(view)) {}

  void step() override {
    // The wrapped view clears its delta on every apply_changes call, so an
    // empty upstream delta correctly yields an empty output
    view_->apply_changes(kInputName, input_fn_());
  }

  void reset() override { view_->reset(); }

  bool has_output() const override { return !view_->get_delta().empty(); }

  const DuckDBZSet &output() const { return view_->get_delta(); }

  // Inside a bigger circuit this node's output is transient — the outer
  // sink owns the view-level delta. (CircuitWrappedView, whose delta
  // surface IS the wrapped buffer, excludes its node from the trim pass.)
  void clear_output() override { view_->drop_delta(); }

  void account_state(StateBytes &out, StateAccounting &acct) const override {
    // The wrapped legacy view (WINDOW/SORT_LIMIT/DISTINCT_ON) is an
    // intermediate operator here — bucket ALL its state as window-class.
    StateBytes tmp;
    view_->account_state(tmp, acct);
    out.window += tmp.total();
  }

  // Checkpointing (Task 2, restore-tail): delegate straight to the wrapped
  // view's own per-node hooks (NativeMaterializedView::circuit_state_kind/
  // serialize_circuit_node_state/restore_circuit_node_state -- see their
  // doc comment in dbsp_duckdb_types.hpp for why these are separate from
  // checkpointable()/serialize_circuit_state()/restore_circuit_state()).
  // This node is a single leaf to the OUTER circuit's checkpoint walk
  // (SingleSourceCircuitView::checkpointable() etc. iterate circuit_'s
  // dbsp::Node objects and call exactly these three methods on each), so
  // whatever the wrapped view reports IS this node's own state_kind.
  StateKind state_kind() const override { return view_->circuit_state_kind(); }

  void serialize_state(std::vector<uint8_t> &out) const override {
    view_->serialize_circuit_node_state(out);
  }

  bool restore_state(const uint8_t *data, size_t len) override {
    return view_->restore_circuit_node_state(data, len);
  }

private:
  InputFn input_fn_;
  std::unique_ptr<NativeMaterializedView> view_;
};

// Batched filter/project node (D1): covers FILTER_EXPR (filters only,
// identity output), MAP_EXPR (projections only), and the IR optimizer's
// fused FILTER_MAP (both). Delta rows are evaluated in DataChunk batches
// through ChunkedEval; projections run over filter survivors only, matching
// the old per-row semantics. Weight-preserving.
class PlanBatchNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  // exprs = filters (num_filters of them) followed by projections; all
  // evaluate against ONE shared input chunk filled once per batch, with
  // survivors selected via DataChunk::Slice (no refill)
  PlanBatchNode(dbsp::NodeId id, InputFn input_fn,
                std::shared_ptr<PlanKeepAlive> keep_alive,
                std::vector<const duckdb::Expression *> filter_exprs,
                std::vector<const duckdb::Expression *> proj_exprs,
                duckdb::vector<duckdb::LogicalType> input_types)
      : dbsp::Node(id, "plan_batch"), input_fn_(std::move(input_fn)),
        num_filters_(filter_exprs.size()) {
    for (const auto *e : proj_exprs) {
      filter_exprs.push_back(e);
    }
    eval_ = std::make_unique<BatchEvaluator>(
        std::move(keep_alive), std::move(filter_exprs),
        std::move(input_types));
  }

  // Pure filter/project over the incoming delta: no durable state.
  StateKind state_kind() const override { return StateKind::STATELESS; }

  void step() override {
    output_.clear();
    const DuckDBZSet &input = input_fn_();

    std::vector<const DuckDBRow *> rows;
    std::vector<int64_t> weights;
    rows.reserve(BatchEvaluator::kBatch);
    weights.reserve(BatchEvaluator::kBatch);

    const size_t num_projs = eval_->expr_count() - num_filters_;

    auto flush = [&]() {
      if (rows.empty()) {
        return;
      }
      const duckdb::idx_t n = rows.size();
      eval_->fill(rows.data(), n);

      // Filters: conjunction over the batch (NULL = fail)
      duckdb::SelectionVector sel(n);
      duckdb::idx_t m = n;
      if (num_filters_ > 0) {
        std::vector<bool> keep(n, true);
        for (size_t f = 0; f < num_filters_; f++) {
          duckdb::Vector &v = eval_->execute(f); // flattened BOOLEAN
          auto &validity = duckdb::FlatVector::Validity(v);
          auto data = duckdb::FlatVector::GetData<bool>(v);
          for (duckdb::idx_t i = 0; i < n; i++) {
            if (keep[i] && (!validity.RowIsValid(i) || !data[i])) {
              keep[i] = false;
            }
          }
        }
        m = 0;
        for (duckdb::idx_t i = 0; i < n; i++) {
          if (keep[i]) {
            sel.set_index(m++, i);
          }
        }
      } else {
        for (duckdb::idx_t i = 0; i < n; i++) {
          sel.set_index(i, i);
        }
      }

      if (num_projs == 0) {
        for (duckdb::idx_t i = 0; i < m; i++) {
          const duckdb::idx_t src = sel.get_index(i);
          output_.insert(*rows[src], weights[src]);
        }
      } else if (m > 0) {
        // Projections over survivors only: slice the already-filled chunk
        if (m < n) {
          eval_->slice(sel, m);
        }
        std::vector<std::vector<duckdb::Value>> out(m);
        for (auto &vals : out) {
          vals.reserve(num_projs);
        }
        for (size_t p = 0; p < num_projs; p++) {
          const size_t e = num_filters_ + p;
          duckdb::Vector &v = eval_->execute(e);
          const auto &type = eval_->return_type(e);
          for (duckdb::idx_t i = 0; i < m; i++) {
            out[i].push_back(BatchEvaluator::read_result(v, type, i));
          }
        }
        // DP3a: the projection result vectors are already flat — fold
        // them into per-row hashes (exact lazy-formula replication, see
        // chunk_row_hashes) so output_.insert never pays the per-Value
        // lazy hash. Each expression owns its result storage, so every
        // slot is still valid here.
        std::vector<size_t> row_hashes(m, 0);
        {
          duckdb::Vector hash_scratch(duckdb::LogicalType::HASH);
          for (size_t p = 0; p < num_projs; p++) {
            fold_vector_hashes(eval_->result_vector(num_filters_ + p), m,
                               hash_scratch, row_hashes);
          }
        }
        for (duckdb::idx_t i = 0; i < m; i++) {
          DuckDBRow row;
          row.columns.assign(std::move(out[i]));
          row.columns.set_hash(row_hashes[i]);
          output_.insert(std::move(row), weights[sel.get_index(i)]);
        }
      }

      rows.clear();
      weights.clear();
    };

    for (const auto &[row, w] : input) {
      rows.push_back(&row);
      weights.push_back(w);
      if (rows.size() == BatchEvaluator::kBatch) {
        flush();
      }
    }
    flush();
    has_output_ = !output_.empty();
  }

  void reset() override {
    output_.clear();
    has_output_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

  void clear_output() override {
    output_.clear();
    has_output_ = false;
  }

private:
  InputFn input_fn_;
  size_t num_filters_;
  std::unique_ptr<BatchEvaluator> eval_;
  DuckDBZSet output_;
  bool has_output_ = false;
};

// Batched MAP_COLS: projects a column selection (GET's column_ids order)
// out of full CDC rows. Replaces the per-row RowMap that paid per-Value
// copies plus a lazily hashed output insert for EVERY source row —
// measured at ~56ms per 100k rows under joins, where the fuse_map_cols
// IR pass cannot elide it (the join needs the projected layout). Output
// values are COW copies of the source row's Values; hashes come from a
// typed chunk fill + fold_vector_hashes (same lazy-formula equality
// contract as DP1/DP3a).
class PlanMapColsNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  PlanMapColsNode(dbsp::NodeId id, InputFn input_fn,
                  std::shared_ptr<PlanKeepAlive> keep_alive,
                  std::vector<duckdb::idx_t> idxs,
                  duckdb::vector<duckdb::LogicalType> out_types)
      : dbsp::Node(id, "plan_scan_cols"), input_fn_(std::move(input_fn)),
        idxs_(std::move(idxs)) {
    eval_ = std::make_unique<BatchEvaluator>(
        std::move(keep_alive), std::vector<const duckdb::Expression *>{},
        std::move(out_types));
  }

  StateKind state_kind() const override { return StateKind::STATELESS; }

  void step() override {
    output_.clear();
    const DuckDBZSet &input = input_fn_();

    std::vector<const DuckDBRow *> rows;
    std::vector<int64_t> weights;
    rows.reserve(BatchEvaluator::kBatch);
    weights.reserve(BatchEvaluator::kBatch);

    const size_t k = idxs_.size();
    std::vector<size_t> row_hashes;
    duckdb::Vector hash_scratch(duckdb::LogicalType::HASH);

    auto flush = [&]() {
      if (rows.empty()) {
        return;
      }
      const duckdb::idx_t n = rows.size();
      // typed fill of the SELECTED columns (native-array fast paths),
      // purely to hash them vectorized
      eval_->fill(rows.data(), n, &idxs_);
      auto &chunk = eval_->input_chunk();
      row_hashes.assign(n, 0);
      for (size_t c = 0; c < k; c++) {
        fold_vector_hashes(chunk.data[c], n, hash_scratch, row_hashes);
      }
      // output values are COW copies of the source Values — no boxing
      for (duckdb::idx_t i = 0; i < n; i++) {
        const auto &cols = rows[i]->columns;
        std::vector<duckdb::Value> vals;
        vals.reserve(k);
        for (size_t c = 0; c < k; c++) {
          vals.push_back(idxs_[c] < cols.size() ? cols[idxs_[c]]
                                                : duckdb::Value());
        }
        DuckDBRow row;
        row.columns.assign(std::move(vals));
        row.columns.set_hash(row_hashes[i]);
        output_.insert(std::move(row), weights[i]);
      }
      rows.clear();
      weights.clear();
    };

    for (const auto &[row, w] : input) {
      rows.push_back(&row);
      weights.push_back(w);
      if (rows.size() == BatchEvaluator::kBatch) {
        flush();
      }
    }
    flush();
    has_output_ = !output_.empty();
  }

  void reset() override {
    output_.clear();
    has_output_ = false;
  }
  bool has_output() const override { return has_output_; }
  const DuckDBZSet &output() const { return output_; }

  void clear_output() override {
    output_.clear();
    has_output_ = false;
  }

private:
  InputFn input_fn_;
  std::vector<duckdb::idx_t> idxs_;
  std::unique_ptr<BatchEvaluator> eval_;
  DuckDBZSet output_;
  bool has_output_ = false;
};

} // namespace dbsp_native
