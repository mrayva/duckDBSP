#pragma once

#include "dbsp_plan_recursive.hpp"

namespace dbsp_native {
// Circuit view built from a translated plan tree. One SourceNode per base
// table (shared across subtrees, e.g. self-joins); apply_changes pushes the
// delta into the matching source and steps the whole circuit once.
class PlannedCircuitView : public NativeMaterializedView {
public:
  using RowSource = dbsp::SourceNode<DuckDBRow, DuckDBRowHash>;
  using RowSink = dbsp::SinkNode<DuckDBRow, DuckDBRowHash>;
  using RowMap =
      dbsp::MapNode<DuckDBRow, DuckDBRow, DuckDBRowHash, DuckDBRowHash>;
  using OutputFn = std::function<const DuckDBZSet &()>;

  PlannedCircuitView(const std::string &name, const std::string &sql,
                     const TableSchema &result_schema,
                     std::shared_ptr<PlanKeepAlive> keep_alive,
                     const PlanOpSpec &root)
      : NativeMaterializedView(name, sql), schema_(result_schema),
        keep_alive_(keep_alive) {
    schema_.table_name = name;
    count_sources(root);
    OutputFn out = build(root, keep_alive);
    sink_ = circuit_.add_node(std::make_unique<RowSink>(
        circuit_.next_node_id(), std::move(out), name_ + "_sink"));
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    auto it = sources_.find(table_name);
    if (it != sources_.end()) {
      it->second->push_borrowed(changes);
    }
    circuit_.step();
    trim_outputs();
    ++version_;
  }

  // One commit, one step: push EVERY updated source's delta before
  // stepping, so a join fed by two same-pass sources (sibling MVs over one
  // base table) sees both sides in a single bilinear step — that is where
  // PlanJoinNode's both-shared −Δl⋈Δr correction fires. Sequential
  // per-source applies would overcount Δl⋈Δr and strand stale rows.
  void apply_changes_batch(
      const std::vector<std::pair<std::string, const DuckDBZSet *>> &changes)
      override {
    batched_ = false; // sink delta covers the whole step
    for (const auto &[src, delta] : changes) {
      auto it = sources_.find(src);
      if (it != sources_.end()) {
        it->second->push_borrowed(*delta);
      }
    }
    circuit_.step();
    trim_outputs();
    ++version_;
  }

  // Bounded-RAM Phase 1a: the sink owns its delta copy, so every other
  // node's output buffer is transient — drop them after each step. The
  // last step's outputs (initial population at worst) used to stay
  // resident for the view's lifetime: 11.4GB of a 23GB wfp footprint.
  void trim_outputs() {
    circuit_.for_each_node([this](dbsp::Node &n) {
      if (&n != static_cast<const dbsp::Node *>(sink_)) {
        n.clear_output();
      }
    });
  }

  void drop_delta() override { sink_->drop_delta(); }

  bool set_table_backed() override {
    sink_->set_table_backed();
    return true;
  }

  const DuckDBZSet &get_result() const override {
    return sink_->materialized();
  }

  void set_result(const DuckDBZSet &result) override {
    sink_->set_materialized(result);
    version_++;
  }

  const DuckDBZSet &get_delta() const override { return sink_->delta(); }

  void account_state(StateBytes &out, StateAccounting &acct) const override {
    NativeMaterializedView::account_state(out, acct); // sink result + delta
    circuit_.for_each_node(
        [&](const dbsp::Node &n) { n.account_state(out, acct); });
  }

  const TableSchema &result_schema() const override { return schema_; }

  std::vector<std::string> source_tables() const override {
    return source_order_;
  }

  // Circuit size; used by IR-optimizer tests to prove rewrites fired
  size_t node_count() const { return circuit_.node_count(); }

  // --- Circuit-state checkpointing (D3b) -------------------------------
  // Same contract as SingleSourceCircuitView: a view is checkpointable iff
  // no node is UNSUPPORTED (value-collecting aggregates, FULL/MARK joins,
  // spilled state — INNER/LEFT/RIGHT joins serialize, see PlanJoinNode::
  // state_kind()). D3b shipped the node-level serialize/restore hooks on
  // PlanAggNode/PlanJoinNode but never overrode these on PlannedCircuitView,
  // so planner-built views (every join/aggregate view since C5) silently
  // fell back to rebuild-by-replay and dbsp_save() wrote no circuit rows.
  bool checkpointable() const override {
    bool ok = true;
    circuit_.for_each_node([&](const dbsp::Node &n) {
      if (n.state_kind() == dbsp::Node::StateKind::UNSUPPORTED) {
        ok = false;
      }
    });
    return ok;
  }

  bool serialize_circuit_state(
      std::vector<std::pair<uint64_t, std::vector<uint8_t>>> &out)
      const override {
    if (!checkpointable()) {
      return false;
    }
    circuit_.for_each_node([&](const dbsp::Node &n) {
      if (n.state_kind() == dbsp::Node::StateKind::SERIALIZABLE) {
        std::vector<uint8_t> blob;
        n.serialize_state(blob);
        out.emplace_back(n.id(), std::move(blob));
      }
    });
    return true;
  }

  bool restore_circuit_state(
      const std::unordered_map<uint64_t, std::vector<uint8_t>> &blobs)
      override {
    // The three failure modes are logged distinctly: the flaky
    // "lazy-restore stash failed to decode" report has never said WHICH
    // of them fired (a checkpointable() flip, a node-id/plan-shape
    // mismatch vs the save-time circuit, or a genuine blob decode
    // failure) — each implicates a completely different cause.
    // DBSP_TEST_CKPT_FLIP: fault injection (tests only) — forces this
    // guard as if checkpointable() flipped between save and realize; no
    // organic SQL sequence can, since the fingerprint gate re-plans the
    // same definition. Checked here only, never at save.
    if (!checkpointable() || std::getenv("DBSP_TEST_CKPT_FLIP") != nullptr) {
      fprintf(stderr,
              "[dbsp] restore_circuit_state(%s): checkpointable() false at "
              "realize (was true at save)\n",
              name().c_str());
      return false;
    }
    bool ok = true;
    circuit_.for_each_node([&](dbsp::Node &n) {
      if (!ok || n.state_kind() != dbsp::Node::StateKind::SERIALIZABLE) {
        return;
      }
      auto it = blobs.find(n.id());
      if (it == blobs.end()) {
        fprintf(stderr,
                "[dbsp] restore_circuit_state(%s): no blob for node id=%llu "
                "'%s' (%zu blobs stashed — plan shape drifted since save?)\n",
                name().c_str(), static_cast<unsigned long long>(n.id()),
                n.name().c_str(), blobs.size());
        ok = false;
        return;
      }
      if (!n.restore_state(it->second.data(), it->second.size())) {
        fprintf(stderr,
                "[dbsp] restore_circuit_state(%s): node id=%llu '%s' rejected "
                "its blob (%zu bytes)\n",
                name().c_str(), static_cast<unsigned long long>(n.id()),
                n.name().c_str(), it->second.size());
        ok = false;
      }
    });
    return ok;
  }

  // I1 shared arrangements: join sides eligible for a shared, CDC-owned
  // arrangement (resolved by CDCManager after sources are tracked)
  const std::vector<ArrangementRequest> &arrangement_requests() const {
    return arrangement_requests_;
  }
  // Tables whose state must NOT be replayed at view initialization: their
  // only consumer is a shared join side whose arrangement is already
  // populated (replaying would double-count through Δl⋈R_arr)
  const std::unordered_set<std::string> &shared_init_skip() const {
    return shared_init_skip_;
  }
  void mark_shared_init_skip(const std::string &table) {
    shared_init_skip_.insert(table);
  }

  void reset() override {
    circuit_.reset();
    version_ = 0;
  }

  // Root ORDER BY/LIMIT: delegate to the embedded sort/limit view so
  // dbsp_query sees rows in ORDER BY order (content identical to the sink)
  void scan(const std::function<void(const DuckDBRow &, Weight)> &callback)
      const override {
    if (ordered_view_) {
      ordered_view_->scan(callback);
      return;
    }
    NativeMaterializedView::scan(callback);
  }

private:
  // Rewrite BoundReference indexes from MAP_COLS output space back to
  // full-table space (index i → column_idxs[i]); false when a ref points
  // past the table (virtual column)
  static bool remap_bound_refs(duckdb::Expression &expr,
                               const std::vector<duckdb::idx_t> &idxs) {
    if (expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_REF) {
      auto &ref = expr.Cast<duckdb::BoundReferenceExpression>();
      if (ref.Index() >= idxs.size()) {
        return false;
      }
      ref.IndexMutable() = idxs[ref.Index()];
      return true;
    }
    bool ok = true;
    duckdb::ExpressionIterator::EnumerateChildren(
        expr, [&](duckdb::Expression &child) {
          ok = ok && remap_bound_refs(child, idxs);
        });
    return ok;
  }

  void count_sources(const PlanOpSpec &spec) {
    if (spec.kind == PlanOpSpec::Kind::SOURCE) {
      source_refs_[spec.table]++;
    }
    for (const auto &child : spec.children) {
      count_sources(*child);
    }
  }

  // Linearity scan over a recursive step's PlanOpSpec subtree. Deliberately
  // separate from count_sources()/source_refs_ (that walk covers the WHOLE
  // plan, used for arrangement sharing) and from step_view->source_tables()
  // (deduped — one entry per distinct table regardless of ref count):
  // neither gives a ref count scoped to one step subtree.
  // PUBLIC: the planner frontend (Walker::visit_recursive_cte) reuses this
  // scan to REJECT row-collapsing recursive steps at translate time.
public:
  struct StepLinearity {
    size_t sentinel_refs = 0;
    bool has_weight_nonlinear_op = false;
  };

  // Counts SOURCE nodes whose table equals `sentinel` (how many times the
  // step references its own working relation) AND flags any operator whose
  // per-row output isn't a linear (weight-preserving-per-row) function of
  // its input — AGGREGATE/DISTINCT/DISTINCT_ON/WINDOW/SORT_LIMIT all
  // collapse or reorder rows in ways that do NOT distribute over
  // step(R+δ) = step(R) + step(δ), even when the sentinel is referenced
  // exactly once. SET_OP is the same class UNLESS it is itself UNION_ALL
  // (a plain per-branch sum, see PlanSetOpNode::multiplicity's UNION_ALL
  // case): UNION/INTERSECT[_ALL]/EXCEPT[_ALL] compute min/clamped-
  // indicator/max(a-b,0) of the per-branch counts, which is nonlinear.
  // The planner frontend REJECTS these shapes inside a recursive step at
  // translate time (Walker::visit_recursive_cte, "row-collapsing
  // operator" unsupported) — they used to be accepted-and-silently-wrong.
  // This scan is that reject's detector, and also stops the signed-delta
  // path from claiming linearity for any such shape that reaches it.
  static void scan_step_linearity(const PlanOpSpec &spec,
                                  const std::string &sentinel,
                                  StepLinearity &info) {
    if (spec.kind == PlanOpSpec::Kind::SOURCE && spec.table == sentinel) {
      info.sentinel_refs++;
    }
    switch (spec.kind) {
    case PlanOpSpec::Kind::AGGREGATE:
    case PlanOpSpec::Kind::DISTINCT:
    case PlanOpSpec::Kind::DISTINCT_ON:
    case PlanOpSpec::Kind::WINDOW:
    case PlanOpSpec::Kind::SORT_LIMIT:
      info.has_weight_nonlinear_op = true;
      break;
    case PlanOpSpec::Kind::SET_OP:
      if (spec.set_op != PlanOpSpec::SetOp::UNION_ALL) {
        info.has_weight_nonlinear_op = true;
      }
      break;
    default:
      break;
    }
    for (const auto &child : spec.children) {
      scan_step_linearity(*child, sentinel, info);
    }
  }

private:
  // v1 eligibility: side is a bare SOURCE referenced exactly once in the
  // whole plan (so init replay can skip that table wholesale) and no more
  // than one side per join shares (keeps init = plain local-side replay).
  // Prefer the right side (conventionally the bigger build side).
  void record_arrangement_request(
      const PlanOpSpec &spec, PlanJoinNode *node,
      const std::vector<const duckdb::Expression *> &lkeys,
      const std::vector<const duckdb::Expression *> &rkeys) {
    // A shared side must be a pure probe target. A side that pads or
    // marks ITSELF (right of RIGHT/FULL, left of LEFT/FULL/MARK) cannot
    // share: init replay skips its table, so its unmatched rows would
    // never be visited and their NULL pads / marks never emitted.
    auto self_padding = [&](bool left) {
      if (left) {
        return spec.join_type == duckdb::JoinType::LEFT ||
               spec.join_type == duckdb::JoinType::OUTER ||
               spec.join_type == duckdb::JoinType::MARK;
      }
      return spec.join_type == duckdb::JoinType::RIGHT ||
             spec.join_type == duckdb::JoinType::OUTER;
    };
    // A side qualifies as a bare scan when it is SOURCE or
    // MAP_COLS(SOURCE) — the projection is folded into the arrangement
    auto scan_of = [](const PlanOpSpec &c) -> const PlanOpSpec * {
      if (c.kind == PlanOpSpec::Kind::SOURCE) {
        return &c;
      }
      if (c.kind == PlanOpSpec::Kind::MAP_COLS &&
          c.children[0]->kind == PlanOpSpec::Kind::SOURCE) {
        return c.children[0].get();
      }
      return nullptr;
    };
    // Bounded-RAM Phase 2 NOTE (attempted, reverted): letting a
    // self-padding LEFT side share looked cheap — reconcile_pads and
    // side_index already read shared_left_ — but init is ORDER-DEPENDENT:
    // with the arrangement backfilled at registration and the left replay
    // not skipped, a source replayed before the left table double-counts
    // matches (L_full⋈Δr now, Δl⋈R again later), and with the replay
    // skipped the pads are never emitted. A correct version needs an
    // explicit init-pads pass in the join node (emit pads for unmatched
    // shared-left rows after init) — tracked in the bounded-RAM spec.
    auto eligible = [&](size_t child) {
      const PlanOpSpec *src = scan_of(*spec.children[child]);
      return src && source_refs_[src->table] == 1 &&
             !self_padding(child == 0);
    };
    const bool right_ok = eligible(1);
    const bool left_ok = eligible(0);
    if (!right_ok && !left_ok) {
      return;
    }
    // Both sides shareable: skip only the RIGHT side's init replay — the
    // left side's full replay ⋈ right arrangement bootstraps the join
    // (skipping both would leave the view empty at init)
    for (const bool left_side : {false, true}) {
      if (left_side ? !left_ok : !right_ok) {
        continue;
      }
      const PlanOpSpec &side = *spec.children[left_side ? 0 : 1];
      const auto &side_keys = left_side ? lkeys : rkeys;
      const PlanOpSpec *src = scan_of(side);

      ArrangementRequest req;
      req.table = src->table;
      // O4: arrangements store FULL table rows; MAP_COLS consumers
      // project bucket rows at probe time and their key expressions are
      // remapped into full-table space, so views with different column
      // needs share one arrangement
      if (side.kind == PlanOpSpec::Kind::MAP_COLS) {
        req.consumer_projection = side.column_idxs;
      }
      req.left_side = left_side;
      req.init_skip = !(left_side && right_ok);
      req.null_safe = spec.null_safe_keys;
      // Probe-target sides never need per-row weights (those serve
      // self-pads/marks, excluded above); marks on a shared RIGHT side
      // still need the total/null-key counters
      req.track_weights = false;
      req.track_counters =
          !left_side && spec.join_type == duckdb::JoinType::MARK;
      bool remap_ok = true;
      if (req.consumer_projection.empty()) {
        req.key_exprs = side_keys;
      } else {
        for (const auto *e : side_keys) {
          auto copy = e->Copy();
          if (!remap_bound_refs(*copy, req.consumer_projection)) {
            remap_ok = false; // key reads a virtual column — don't share
            break;
          }
          req.key_exprs.push_back(copy.get());
          keep_alive_->rewritten_exprs.push_back(std::move(copy));
        }
      }
      if (!remap_ok) {
        continue;
      }
      req.side_types = src->input_types;
      req.keep_alive = keep_alive_;
      req.node = node;
      finish_request(std::move(req));
    }
  }

  void finish_request(ArrangementRequest req) {
    const auto &key_exprs = req.key_exprs;
    std::string fp = req.table;
    fp += req.null_safe ? "|ns1" : "|ns0";
    fp += req.track_weights ? "|w1" : "|w0";
    fp += req.track_counters ? "|c1" : "|c0";
    // O4: no projection component — keys are canonical full-space
    for (const auto *e : key_exprs) {
      fp += "|";
      fp += e->ToString();
    }
    req.fingerprint = std::move(fp);
    arrangement_requests_.push_back(std::move(req));
  }


  OutputFn build(const PlanOpSpec &spec,
                 const std::shared_ptr<PlanKeepAlive> &keep_alive) {
    switch (spec.kind) {
    case PlanOpSpec::Kind::SOURCE: {
      auto it = sources_.find(spec.table);
      RowSource *src;
      if (it != sources_.end()) {
        src = it->second;
      } else {
        src = circuit_.add_node(
            std::make_unique<RowSource>(circuit_.next_node_id(), spec.table));
        sources_[spec.table] = src;
        source_order_.push_back(spec.table);
      }
      return [src]() -> const DuckDBZSet & { return src->output(); };
    }
    case PlanOpSpec::Kind::MAP_COLS: {
      OutputFn child = build(*spec.children[0], keep_alive);
      auto idxs = spec.column_idxs;
      if (!spec.input_types.empty()) {
        // batched projection with vectorized output hashing (virtual /
        // out-of-range entries fill NULL; BOOLEAN is a placeholder chunk
        // type — the column is all-NULL and NULLs hash as kNullHash
        // regardless of type)
        duckdb::vector<duckdb::LogicalType> out_types;
        for (auto idx : idxs) {
          out_types.push_back(idx < spec.input_types.size()
                                  ? spec.input_types[idx]
                                  : duckdb::LogicalType::BOOLEAN);
        }
        auto *node = circuit_.add_node(std::make_unique<PlanMapColsNode>(
            circuit_.next_node_id(), std::move(child), keep_alive,
            std::move(idxs), std::move(out_types)));
        return [node]() -> const DuckDBZSet & { return node->output(); };
      }
      auto *node = circuit_.add_node(std::make_unique<RowMap>(
          circuit_.next_node_id(), std::move(child),
          [idxs](const DuckDBRow &row) {
            DuckDBRow out;
            out.columns.reserve(idxs.size());
            for (auto idx : idxs) {
              out.columns.push_back(idx < row.columns.size()
                                        ? row.columns[idx]
                                        : duckdb::Value());
            }
            return out;
          },
          "plan_scan_cols"));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::FILTER_EXPR: {
      OutputFn child = build(*spec.children[0], keep_alive);
      auto *node = circuit_.add_node(std::make_unique<PlanBatchNode>(
          circuit_.next_node_id(), std::move(child), keep_alive, spec.exprs,
          std::vector<const duckdb::Expression *>{}, spec.input_types));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::MAP_EXPR: {
      OutputFn child = build(*spec.children[0], keep_alive);
      auto *node = circuit_.add_node(std::make_unique<PlanBatchNode>(
          circuit_.next_node_id(), std::move(child), keep_alive,
          std::vector<const duckdb::Expression *>{}, spec.exprs,
          spec.input_types));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::AGGREGATE: {
      OutputFn child = build(*spec.children[0], keep_alive);
      std::vector<const duckdb::Expression *> arg_exprs;
      std::vector<PlanAggregateNode::AggInstance> aggs;
      for (const auto &agg_spec : spec.agg_specs) {
        PlanAggregateNode::AggInstance inst;
        inst.fn = agg_spec.fn;
        inst.integer_arg = agg_spec.integer_arg;
        inst.decimal_arg = agg_spec.decimal_arg;
        inst.decimal_scale = agg_spec.decimal_scale;
        inst.return_type = agg_spec.return_type;
        inst.distinct = agg_spec.distinct;
        inst.separator = agg_spec.separator;
        inst.quantile = agg_spec.quantile;
        if (agg_spec.arg) {
          inst.arg_idx = static_cast<int>(arg_exprs.size());
          arg_exprs.push_back(agg_spec.arg);
        }
        if (agg_spec.filter) {
          inst.filter_idx = static_cast<int>(arg_exprs.size());
          arg_exprs.push_back(agg_spec.filter);
        }
        for (const auto &key : agg_spec.order_keys) {
          inst.order_idxs.push_back(static_cast<int>(arg_exprs.size()));
          inst.order_dirs.emplace_back(key.ascending, key.nulls_first);
          arg_exprs.push_back(key.expr);
        }
        aggs.push_back(std::move(inst));
      }
      auto *node = circuit_.add_node(std::make_unique<PlanAggregateNode>(
          circuit_.next_node_id(), std::move(child), keep_alive, spec.exprs,
          std::move(arg_exprs), std::move(aggs), spec.input_types));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::JOIN: {
      OutputFn left = build(*spec.children[0], keep_alive);
      OutputFn right = build(*spec.children[1], keep_alive);
      std::vector<PlanJoinNode::KeyPair> keys;
      for (const auto &cond : spec.equi_conds) {
        PlanJoinNode::KeyPair kp;
        kp.left = std::make_unique<RowExprEval>(keep_alive, *cond.left,
                                                spec.left_types);
        kp.right = std::make_unique<RowExprEval>(keep_alive, *cond.right,
                                                 spec.right_types);
        keys.push_back(std::move(kp));
      }
      std::vector<PlanJoinNode::Residual> residuals;
      for (const auto &cond : spec.residual_conds) {
        PlanJoinNode::Residual res;
        res.left = std::make_unique<RowExprEval>(keep_alive, *cond.left,
                                                 spec.left_types);
        res.right = std::make_unique<RowExprEval>(keep_alive, *cond.right,
                                                  spec.right_types);
        res.cmp = cond.cmp;
        residuals.push_back(std::move(res));
      }
      std::vector<const duckdb::Expression *> lkeys, rkeys;
      for (const auto &cond : spec.equi_conds) {
        lkeys.push_back(cond.left);
        rkeys.push_back(cond.right);
      }
      auto lkeys_copy = lkeys;
      auto rkeys_copy = rkeys;
      auto *node = circuit_.add_node(std::make_unique<PlanJoinNode>(
          circuit_.next_node_id(), std::move(left), std::move(right),
          std::move(keys), std::move(residuals), spec.join_type,
          spec.left_types, spec.right_types, spec.null_safe_keys,
          keep_alive, std::move(lkeys), std::move(rkeys)));
      record_arrangement_request(spec, node, lkeys_copy, rkeys_copy);
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::DISTINCT: {
      OutputFn child = build(*spec.children[0], keep_alive);
      auto *node = circuit_.add_node(std::make_unique<PlanDistinctNode>(
          circuit_.next_node_id(), std::move(child)));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::SET_OP: {
      std::vector<PlanSetOpNode::InputFn> inputs;
      for (const auto &child : spec.children) {
        inputs.push_back(build(*child, keep_alive));
      }
      auto *node = circuit_.add_node(std::make_unique<PlanSetOpNode>(
          circuit_.next_node_id(), std::move(inputs), spec.set_op));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::WINDOW: {
      OutputFn child = build(*spec.children[0], keep_alive);
      TableSchema source_schema;
      source_schema.table_name = EmbeddedViewNode::kInputName;
      source_schema.columns = spec.window_source_cols;
      TableSchema window_schema;
      window_schema.table_name = name_ + "_window";
      window_schema.columns = spec.window_result_cols;
      auto view = std::make_unique<NativeWindowView>(
          name_ + "_window", "", EmbeddedViewNode::kInputName, window_schema,
          source_schema, spec.window_defs);
      auto *node = circuit_.add_node(std::make_unique<EmbeddedViewNode>(
          circuit_.next_node_id(), std::move(child), std::move(view)));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::CTE: {
      // Build the definition once; all CTE_REFs share its output
      cte_outputs_[spec.cte_index] = build(*spec.children[0], keep_alive);
      return build(*spec.children[1], keep_alive);
    }
    case PlanOpSpec::Kind::CTE_REF:
      return cte_outputs_.at(spec.cte_index);
    case PlanOpSpec::Kind::FILTER_MAP: {
      OutputFn child = build(*spec.children[0], keep_alive);
      auto *node = circuit_.add_node(std::make_unique<PlanBatchNode>(
          circuit_.next_node_id(), std::move(child), keep_alive,
          spec.filter_exprs, spec.exprs, spec.input_types));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::DELIM_JOIN: {
      OutputFn left = build(*spec.children[0], keep_alive);
      // DISTINCT of the correlated key columns, shared by every DELIM_GET
      // in the right subplan (incremental: emits key deltas as the set of
      // distinct outer keys changes)
      auto *keys_node = circuit_.add_node(std::make_unique<PlanBatchNode>(
          circuit_.next_node_id(), left, keep_alive,
          std::vector<const duckdb::Expression *>{}, spec.exprs,
          spec.left_types));
      auto *distinct_node =
          circuit_.add_node(std::make_unique<PlanDistinctNode>(
              circuit_.next_node_id(),
              [keys_node]() -> const DuckDBZSet & {
                return keys_node->output();
              },
              "plan_delim_keys"));
      delim_stack_.push_back([distinct_node]() -> const DuckDBZSet & {
        return distinct_node->output();
      });
      OutputFn right = build(*spec.children[1], keep_alive);
      delim_stack_.pop_back();

      std::vector<PlanJoinNode::KeyPair> keys;
      for (const auto &cond : spec.equi_conds) {
        PlanJoinNode::KeyPair kp;
        kp.left = std::make_unique<RowExprEval>(keep_alive, *cond.left,
                                                spec.left_types);
        kp.right = std::make_unique<RowExprEval>(keep_alive, *cond.right,
                                                 spec.right_types);
        keys.push_back(std::move(kp));
      }
      std::vector<const duckdb::Expression *> lkeys, rkeys;
      for (const auto &cond : spec.equi_conds) {
        lkeys.push_back(cond.left);
        rkeys.push_back(cond.right);
      }
      auto *node = circuit_.add_node(std::make_unique<PlanJoinNode>(
          circuit_.next_node_id(), std::move(left), std::move(right),
          std::move(keys), std::vector<PlanJoinNode::Residual>{},
          spec.join_type, spec.left_types, spec.right_types,
          spec.null_safe_keys, keep_alive, std::move(lkeys),
          std::move(rkeys), "plan_delim_join"));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::DELIM_REF:
      return delim_stack_.back();
    case PlanOpSpec::Kind::DISTINCT_ON: {
      OutputFn child = build(*spec.children[0], keep_alive);
      TableSchema vschema;
      vschema.table_name = name_ + "_distinct_on";
      std::vector<size_t> keys;
      keys.reserve(spec.column_idxs.size());
      for (auto idx : spec.column_idxs) {
        keys.push_back(static_cast<size_t>(idx));
      }
      std::vector<NativeDistinctOnView::SortColumn> order;
      order.reserve(spec.sort_columns.size());
      for (const auto &sc : spec.sort_columns) {
        order.push_back({sc.column_idx, sc.ascending, sc.nulls_first});
      }
      auto view = std::make_unique<NativeDistinctOnView>(
          name_ + "_distinct_on", "", EmbeddedViewNode::kInputName, vschema,
          std::move(keys), std::move(order));
      auto *node = circuit_.add_node(std::make_unique<EmbeddedViewNode>(
          circuit_.next_node_id(), std::move(child), std::move(view)));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::REC_CTE: {
      OutputFn anchor = build(*spec.children[0], keep_alive);
      std::string sentinel =
          "__rec_cte_" + std::to_string(spec.cte_index) + "__";
      // Linearity: the recursive relation referenced exactly once in the
      // STEP subtree only (children[1]; the anchor doesn't recurse), AND no
      // weight-nonlinear operator (AGGREGATE/DISTINCT/DISTINCT_ON/WINDOW/
      // SORT_LIMIT/non-UNION-ALL SET_OP) anywhere in that subtree.
      StepLinearity step_linearity;
      scan_step_linearity(*spec.children[1], sentinel, step_linearity);
      bool linear_step = step_linearity.sentinel_refs == 1 &&
                         !step_linearity.has_weight_nonlinear_op;
      TableSchema step_schema;
      step_schema.table_name = name_ + "_rec_step";
      auto step_view = std::make_unique<PlannedCircuitView>(
          name_ + "_rec_step", "", step_schema, keep_alive,
          *spec.children[1]);
      // The inner view routes by source name; the outer circuit must own a
      // SourceNode for every base table the step reads (CDC pushes deltas
      // into outer sources only). The sentinel stays internal.
      std::vector<std::pair<std::string, PlanRecursiveNode::InputFn>>
          base_inputs;
      for (const auto &t : step_view->source_tables()) {
        if (t == sentinel) {
          continue;
        }
        PlanOpSpec src;
        src.kind = PlanOpSpec::Kind::SOURCE;
        src.table = t;
        base_inputs.emplace_back(t, build(src, keep_alive));
      }
      bool union_all = spec.set_op == PlanOpSpec::SetOp::UNION_ALL;
      auto *node = circuit_.add_node(std::make_unique<PlanRecursiveNode>(
          circuit_.next_node_id(), std::move(anchor), std::move(step_view),
          sentinel, union_all, std::move(base_inputs),
          /*max_iterations=*/1000, linear_step));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::SORT_LIMIT: {
      OutputFn child = build(*spec.children[0], keep_alive);
      TableSchema vschema;
      vschema.table_name = name_ + "_sortlimit";
      NativeSortView::ProjectFn project = nullptr;
      if (!spec.project_idxs.empty()) {
        auto idxs = spec.project_idxs;
        project = [idxs](const DuckDBRow &row) {
          DuckDBRow out;
          out.columns.reserve(idxs.size());
          for (auto i : idxs) {
            out.columns.push_back(i < row.columns.size() ? row.columns[i]
                                                         : duckdb::Value());
          }
          return out;
        };
      }
      std::unique_ptr<NativeMaterializedView> view;
      if (spec.limit >= 0 || spec.offset > 0 || spec.limit_percent >= 0) {
        view = std::make_unique<NativeLimitView>(
            name_ + "_limit", "", EmbeddedViewNode::kInputName, vschema,
            spec.limit, spec.offset, spec.sort_columns, project,
            spec.limit_percent);
      } else {
        view = std::make_unique<NativeSortView>(
            name_ + "_sort", "", EmbeddedViewNode::kInputName, vschema,
            spec.sort_columns, project);
      }
      if (spec.presentation_root) {
        ordered_view_ = view.get();
      }
      auto *node = circuit_.add_node(std::make_unique<EmbeddedViewNode>(
          circuit_.next_node_id(), std::move(child), std::move(view)));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    }
    // Unreachable; keeps compilers happy
    static DuckDBZSet empty;
    return []() -> const DuckDBZSet & { return empty; };
  }

  dbsp::Circuit circuit_;
  TableSchema schema_;
  std::unordered_map<std::string, RowSource *> sources_;
  std::vector<std::string> source_order_;
  std::unordered_map<duckdb::idx_t, OutputFn> cte_outputs_;
  std::vector<OutputFn> delim_stack_; // enclosing DELIM joins' key outputs
  std::shared_ptr<PlanKeepAlive> keep_alive_;
  std::unordered_map<std::string, int> source_refs_; // SOURCE count per table
  std::vector<ArrangementRequest> arrangement_requests_;
  std::unordered_set<std::string> shared_init_skip_;
  RowSink *sink_ = nullptr;
  // Embedded sort/limit view at the plan root; owned by its EmbeddedViewNode
  NativeMaterializedView *ordered_view_ = nullptr;
};

} // namespace dbsp_native
