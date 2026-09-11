#pragma once

#include "dbsp_plan_nodes.hpp"

namespace dbsp_native {
// Fixed-point driver for WITH RECURSIVE. The anchor is computed inline in
// the outer circuit; the recursive step runs as an inner view (a nested
// PlannedCircuitView) whose self-reference is a source named `sentinel`.
//
// Insert-only deltas are handled incrementally: seed the frontier with the
// anchor delta plus the step's reaction to base-table deltas, then iterate
// the step on the frontier until it stops producing new rows (UNION dedup
// state persists across calls, so later deltas cannot double-count).
//
// A LINEAR UNION ALL step (the recursive relation referenced exactly once
// in the step subtree, `linear_step_`) is linear in that relation:
// step(R + δ) = step(R) + step(δ). Retraction-bearing deltas therefore ride
// the SAME incremental fixpoint as insertions, just with signed weights —
// no deletion special-case needed (design: docs/superpowers/specs/
// 2026-07-31-linear-recursion-deltas-design.md). `admit`'s union_all_
// branch already does plain multiset arithmetic on `accumulated_` (insert
// signed weight; a row whose weight nets to zero is erased by ZSet::insert
// itself); `iterate` pushes signed frontiers through step_view_ unchanged
// (join/filter/project are all weight-linear). A max_iterations_ trip
// aborts the signed attempt and falls back to recompute() from the
// snapshot taken before admission — never a partial delta (logged under
// DBSP_DEBUG_SYNC).
//
// All other deletion-bearing shapes are unchanged: NONLINEAR UNION ALL
// (recursive relation referenced ≥2 times) keeps recompute() — weighted
// deletion through a self-join doesn't distribute. UNION (set-semantics)
// recursion takes the DRed (Delete-Rederive) path: an incremental
// overdelete fixpoint over-approximates the retraction, then a rederive
// fixpoint re-admits rows that still have alternative support (cycles
// included). Overdelete is O(affected subgraph); rederive costs one image
// pass over the surviving relation plus a bounded rederive fixpoint
// (deeper multi-hop re-admission) — still cheaper than the full recompute
// it replaces. recompute() is retained for the nonlinear path and as the
// differential-test oracle for all shapes.
//
// Test-only observability hook (Task 1, linear-recursion-deltas): counts
// PlanRecursiveNode::recompute() invocations process-wide, following the
// existing g_plan_ir_optimize/g_intraop_shards convention of a directly
// test-accessible inline atomic rather than adding new SQL surface. Tests
// read a before/after delta around the mutation under test.
inline std::atomic<size_t> g_recompute_invocations{0};

class PlanRecursiveNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  PlanRecursiveNode(dbsp::NodeId id, InputFn anchor,
                    std::unique_ptr<NativeMaterializedView> step_view,
                    std::string sentinel, bool union_all,
                    std::vector<std::pair<std::string, InputFn>> base_inputs,
                    size_t max_iterations = 1000, bool linear_step = false)
      : dbsp::Node(id, "plan_recursive"), anchor_(std::move(anchor)),
        step_view_(std::move(step_view)), sentinel_(std::move(sentinel)),
        union_all_(union_all), base_inputs_(std::move(base_inputs)),
        max_iterations_(max_iterations), linear_step_(linear_step) {
    // Test-only override: DBSP_REC_MAX_ITER lets integration tests force a
    // small iteration cap so the max_iterations_ fallback (signed path ->
    // recompute()) can be exercised deterministically without needing a
    // naturally-deep chain. Read once at construction, not per-step.
    if (const char *env = std::getenv("DBSP_REC_MAX_ITER")) {
      char *end = nullptr;
      unsigned long v = std::strtoul(env, &end, 10);
      if (end != env && v > 0) {
        max_iterations_ = v;
      }
    }
  }

  void step() override {
    output_.clear();

    // Integrate inputs; detect deletions
    const DuckDBZSet &anchor_delta = anchor_();
    bool has_deletion = false;
    for (const auto &[row, w] : anchor_delta) {
      anchor_total_.insert(row, w);
      has_deletion |= w < 0;
    }
    std::vector<std::pair<std::string, const DuckDBZSet *>> base_deltas;
    for (auto &[table, fn] : base_inputs_) {
      const DuckDBZSet &d = fn();
      if (d.empty()) {
        continue;
      }
      auto &total = base_totals_[table];
      for (const auto &[row, w] : d) {
        total.insert(row, w);
        has_deletion |= w < 0;
      }
      base_deltas.emplace_back(table, &d);
    }

    // Linear UNION ALL steps skip the deletion special-case entirely: the
    // incremental path below is generalized to signed weights and handles
    // insertions and retractions uniformly (see class-header design note).
    const bool signed_path = union_all_ && linear_step_;
    if (has_deletion && !signed_path) {
      if (union_all_) {
        recompute(); // nonlinear multiplicity semantics: full recompute
      } else {
        dred(anchor_delta, base_deltas);
      }
      has_output_ = !output_.empty();
      return;
    }

    // Incremental path: insert-only (any shape) or signed retractions for
    // a linear UNION ALL step. Weights carry sign as-is; `admit`'s
    // union_all_ branch does plain multiset arithmetic on `accumulated_`.
    DuckDBZSet seed = anchor_delta;
    for (const auto &[table, d] : base_deltas) {
      step_view_->apply_changes(table, *d);
      for (const auto &[row, w] : step_view_->get_delta()) {
        seed.insert(row, w);
      }
    }

    if (has_deletion) {
      // Signed retraction path (union_all_ && linear_step_, guaranteed by
      // the guard above). Snapshot first: a max_iterations_ trip OR any
      // exception during admission/iteration must recover to the pre-delta
      // state and fall back to recompute() — never emit a partial delta
      // (Rule 12) — the same shape as dred()'s recovery guard. Never taken
      // on the pre-existing insert-only path below, so ordinary edits pay
      // no extra copy.
      DuckDBZSet accumulated_snapshot = accumulated_;
      DuckDBZSet output_snapshot = output_;
      bool converged = false;
      bool threw = false;
      try {
        DuckDBZSet frontier;
        for (const auto &[row, w] : seed) {
          if (w != 0) {
            admit(row, w, frontier, output_);
          }
        }
        converged = iterate(frontier, output_);
      } catch (...) {
        threw = true;
      }
      if (threw || !converged) {
        accumulated_ = std::move(accumulated_snapshot);
        output_ = std::move(output_snapshot);
        recompute();
        if (std::getenv("DBSP_DEBUG_SYNC")) {
          std::cerr << "[dbsp] recursive: linear signed path "
                    << (threw ? "threw" : "hit max_iterations_")
                    << ", falling back to recompute()\n";
        }
      }
      has_output_ = !output_.empty();
      return;
    }

    // Pre-existing insert-only path (any shape, including nonlinear/UNION
    // recursion when this delta happens to carry no deletion): unchanged.
    DuckDBZSet frontier;
    for (const auto &[row, w] : seed) {
      if (w > 0) {
        admit(row, w, frontier, output_);
      }
    }
    iterate(frontier, output_);
    has_output_ = !output_.empty();
  }

  void reset() override {
    accumulated_.clear();
    anchor_total_.clear();
    base_totals_.clear();
    output_.clear();
    has_output_ = false;
    step_view_->reset();
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

  void clear_output() override {
    output_.clear();
    has_output_ = false;
  }

  void account_state(StateBytes &out, StateAccounting &acct) const override {
    out.recursion += acct.zset_bytes(accumulated_);
    out.other += acct.zset_bytes(output_);
    StateBytes tmp;
    step_view_->account_state(tmp, acct); // step circuit's own state
    out.recursion += tmp.total();
  }

  // Checkpointing (Task 1, restore-tail): SERIALIZABLE iff union_all_ —
  // UNION (set-semantics) recursion's DRed support_/dred bookkeeping is not
  // captured this pass, so it stays UNSUPPORTED, the same decline
  // discipline as every other unbuilt shape — AND the nested step circuit
  // is itself wholly checkpointable (PlannedCircuitView::checkpointable(),
  // the same gate save_checkpoint() already applies to every top-level
  // view; a step containing e.g. a spilled join or any other UNSUPPORTED
  // node declines here too). This is independent of linear_step_: the
  // fallback recompute() path (taken for a max_iterations_ trip, any
  // exception during signed admission, or any deletion on a NONLINEAR
  // union_all step) rebuilds entirely from anchor_total_/base_totals_ +
  // step_view_->reset(), so a restored node resumes correctly there
  // whether or not the step is linear — only the SIGNED retraction path
  // additionally needs linear_step_, and that gate is unchanged, evaluated
  // at runtime per commit inside step().
  StateKind state_kind() const override {
    if (!union_all_) {
      return StateKind::UNSUPPORTED;
    }
    return step_view_->checkpointable() ? StateKind::SERIALIZABLE
                                        : StateKind::UNSUPPORTED;
  }

  // serialize_state/restore_state carry exactly what step()/recompute()
  // read to resume:
  //   - accumulated_: the integrated fixed-point result. Its content is
  //     identical to this node's own materialized output (== the outer
  //     view's get_result() at save time, restored separately at the view
  //     level), so recompute()'s diff-against-old-accumulated is correct on
  //     the first post-restore call, and admit()'s union_all_ branch keeps
  //     accumulating into the SAME running total future commits expect.
  //   - anchor_total_ / base_totals_: the running integrated totals
  //     recompute() reseeds its whole-fixpoint rebuild from, and that every
  //     future step() (either path) keeps accumulating into regardless of
  //     shape — needed so a recompute() several commits after restore still
  //     integrates the FULL history, not just what changed post-restore.
  //   - step_view_'s own per-node state (equi-key indexes / group state —
  //     whatever its apply_changes' incremental maintenance needs to
  //     resume), via its EXISTING serialize_circuit_state/
  //     restore_circuit_state, embedded here as a length-prefixed sub-blob
  //     keyed by inner node id. state_kind() only reports SERIALIZABLE when
  //     step_view_->checkpointable(), so serialize_circuit_state cannot
  //     decline when this runs.
  // Excluded, and why: output_/has_output_ (checkpoints are taken
  // quiescent — no pending delta to resume, same convention SourceNode and
  // every other checkpointed node already follow); anchor_/step_view_'s
  // object identity/sentinel_/union_all_/base_inputs_/max_iterations_/
  // linear_step_ (view-definition-derived construction-time configuration,
  // not data — rebuilt fresh by the normal cold-construction path that
  // always runs BEFORE restore_state() is ever called, per
  // restore_view_state's contract, and separately guarded by the
  // checkpoint's SQL fingerprint against a changed definition);
  // g_recompute_invocations (a process-wide test-only counter, not node
  // state at all).
  void serialize_state(std::vector<uint8_t> &out) const override {
    BlobWriter w;
    auto write_zset = [&w](const DuckDBZSet &z) {
      w.u64(z.size());
      for (const auto &[row, weight] : z) {
        w.row(row.columns);
        w.i64(weight);
      }
    };
    write_zset(accumulated_);
    write_zset(anchor_total_);
    w.u64(base_totals_.size());
    for (const auto &[table, total] : base_totals_) {
      w.row(std::vector<duckdb::Value>{duckdb::Value(table)});
      write_zset(total);
    }
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> inner;
    step_view_->serialize_circuit_state(inner);
    w.u64(inner.size());
    for (const auto &[node_id, blob] : inner) {
      w.u64(node_id);
      w.row(std::vector<duckdb::Value>{
          duckdb::Value::BLOB(blob.data(), blob.size())});
    }
    out = w.take();
  }

  bool restore_state(const uint8_t *data, size_t len) override {
    try {
      BlobReader r(data, len);
      auto read_zset = [&r](DuckDBZSet &z) {
        z.clear();
        const uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
          DuckDBRow row = r.hashed_row();
          const int64_t weight = r.i64();
          z.insert(row, weight);
        }
      };
      read_zset(accumulated_);
      read_zset(anchor_total_);
      base_totals_.clear();
      const uint64_t n_tables = r.u64();
      for (uint64_t i = 0; i < n_tables; i++) {
        auto key = r.row();
        if (key.size() != 1) {
          return false;
        }
        const std::string table = duckdb::StringValue::Get(key[0]);
        DuckDBZSet total;
        read_zset(total);
        base_totals_.emplace(table, std::move(total));
      }
      std::unordered_map<uint64_t, std::vector<uint8_t>> inner_blobs;
      const uint64_t n_inner = r.u64();
      for (uint64_t i = 0; i < n_inner; i++) {
        const uint64_t node_id = r.u64();
        auto blob_val = r.row();
        if (blob_val.size() != 1) {
          return false;
        }
        const auto &bytes = duckdb::StringValue::Get(blob_val[0]);
        inner_blobs.emplace(node_id,
                            std::vector<uint8_t>(bytes.begin(), bytes.end()));
      }
      if (!step_view_->restore_circuit_state(inner_blobs)) {
        return false;
      }
      return r.done();
    } catch (...) {
      return false;
    }
  }

private:
  // Re-run the whole fixed point from integrated inputs; output_ becomes
  // the diff against the previous accumulated state
  void recompute() {
    g_recompute_invocations.fetch_add(1, std::memory_order_relaxed);
    DuckDBZSet old_accumulated = std::move(accumulated_);
    accumulated_ = DuckDBZSet();
    step_view_->reset();

    DuckDBZSet seed = anchor_total_;
    for (const auto &[table, total] : base_totals_) {
      if (total.empty()) {
        continue;
      }
      step_view_->apply_changes(table, total);
      for (const auto &[row, w] : step_view_->get_delta()) {
        seed.insert(row, w);
      }
    }
    DuckDBZSet scratch; // recompute emits via diff, not via admit
    DuckDBZSet frontier;
    for (const auto &[row, w] : seed) {
      if (w > 0) {
        admit(row, w, frontier, scratch);
      }
    }
    iterate(frontier, scratch);

    for (const auto &[row, w] : accumulated_) {
      output_.insert(row, w);
    }
    for (const auto &[row, w] : old_accumulated) {
      output_.insert(row, -w);
    }
  }

  // Delete-Rederive for set-semantics (UNION) recursion. Overdelete
  // over-approximates the retraction; rederive restores rows that still
  // have alternative support. Maintains the invariant
  //   accumulated_ (set) == step_view_ sentinel arrangement
  // by feeding every accumulated_ change to the sentinel source. output_
  // is built incrementally (no full diff). See docs spec 2026-07-07.
  void dred(const DuckDBZSet &anchor_delta,
            const std::vector<std::pair<std::string, const DuckDBZSet *>>
                &base_deltas) {
    // Snapshot the true pre-delta state so an iteration-cap exhaustion can
    // recover safely and loudly (Rule 12) instead of silently desyncing the
    // accumulated_ == sentinel invariant. At entry accumulated_ is untouched
    // and output_ is empty (step() cleared it), so these snapshots are the
    // exact state recompute() must rebuild from.
    DuckDBZSet accumulated_snapshot = accumulated_;
    DuckDBZSet output_snapshot = output_;
    // Restore the true pre-delta state and rebuild via the self-healing full
    // recompute (which resets step_view_ and re-derives from integrated
    // inputs). Turns a silent invariant desync into a correct-but-slower diff.
    auto recover_via_recompute = [&]() {
      accumulated_ = std::move(accumulated_snapshot);
      output_ = std::move(output_snapshot);
      recompute();
    };

    // --- Seed: split inputs into retractions (drive overdelete) and
    //     insertions (seed rederive). ---
    DuckDBZSet frontier; // rows being retracted this round (weight 1 each)
    auto retract = [&](const DuckDBRow &row) {
      if (accumulated_.get(row) != 0) {
        accumulated_.insert(row, -1); // 1 + (-1) = 0 → removed
        output_.insert(row, -1);
        frontier.insert(row, 1);
      }
    };
    // Positive rows in the delta are NOT force-admitted here: their support
    // was computed against the pre-overdelete sentinel, so seeding them
    // directly can admit phantoms whose support is later over-deleted. Every
    // legitimately-new row is recovered by the phantom-free paths below —
    // the I(survivors) image, the anchor_total_ re-seed, and the fwd
    // fixpoint. So classify only drives retractions.
    auto classify = [&](const DuckDBZSet &z) {
      for (const auto &[row, w] : z) {
        if (w < 0)
          retract(row);
      }
    };
    classify(anchor_delta);
    for (const auto &[table, d] : base_deltas) {
      step_view_->apply_changes(table, *d); // base delta already integrated
      classify(step_view_->get_delta());    // derived reactions
    }

    // --- Overdelete: iterate the retraction frontier through the step. ---
    size_t iter = 0;
    while (!frontier.empty() && iter++ < max_iterations_) {
      DuckDBZSet neg;
      for (const auto &[row, w] : frontier)
        neg.insert(row, -w); // retract frontier from sentinel arrangement
      step_view_->apply_changes(sentinel_, neg);
      DuckDBZSet next;
      for (const auto &[row, w] : step_view_->get_delta()) {
        if (w < 0 && accumulated_.get(row) != 0) {
          accumulated_.insert(row, -1);
          output_.insert(row, -1);
          next.insert(row, 1);
        }
      }
      frontier = std::move(next);
    }
    // Loop exits either on an empty frontier (natural) or on the iteration
    // cap. A non-empty frontier here means the cap was hit with work pending
    // and the incremental invariant is unreliable — recover and bail.
    if (!frontier.empty()) {
      recover_via_recompute();
      return;
    }

    // --- Rederive: recompute the step's one-step image over survivors, then
    //     iterate; re-admit any over-deleted row that reappears. ---
    // Clear the sentinel side (survivors → empty), then re-present survivors
    // so the resulting get_delta() is the full image I(survivors). Base
    // arrangements are untouched, so this is a linear round-trip.
    DuckDBZSet survivors = accumulated_;
    {
      DuckDBZSet clear;
      for (const auto &[row, w] : survivors)
        clear.insert(row, -1);
      step_view_->apply_changes(sentinel_, clear); // sentinel → empty (discard)
    }
    DuckDBZSet fwd;
    step_view_->apply_changes(sentinel_, survivors); // sentinel → survivors
    for (const auto &[row, w] : step_view_->get_delta()) {
      if (w > 0 && accumulated_.get(row) == 0) {
        accumulated_.insert(row, 1);
        output_.insert(row, 1);
        fwd.insert(row, 1);
      }
    }
    // Base facts (anchor_total_, the non-recursive UNION arm) are
    // unconditional support: a row that is still directly present in the
    // base term must be re-admitted even when it was not touched by this
    // round's delta. Cyclic recursion can overdelete a row that is
    // simultaneously anchor-supported (the join-derived path and the base
    // fact coincide in value), and nothing else would re-seed it.
    for (const auto &[row, w] : anchor_total_) {
      if (w > 0 && accumulated_.get(row) == 0) {
        accumulated_.insert(row, 1);
        output_.insert(row, 1);
        fwd.insert(row, 1);
      }
    }
    // Deeper rederivations: rederived rows are now part of the recursive
    // relation and may rederive further over-deleted rows.
    iter = 0;
    while (!fwd.empty() && iter++ < max_iterations_) {
      step_view_->apply_changes(sentinel_, fwd);
      DuckDBZSet next;
      for (const auto &[row, w] : step_view_->get_delta()) {
        if (w > 0 && accumulated_.get(row) == 0) {
          accumulated_.insert(row, 1);
          output_.insert(row, 1);
          next.insert(row, 1);
        }
      }
      fwd = std::move(next);
    }
    // Same guard for the rederive fixpoint: a non-empty fwd means the cap
    // was hit before convergence.
    if (!fwd.empty()) {
      recover_via_recompute();
      return;
    }
  }

  // Push `frontier` through step_view_ to a fixed point, admitting each
  // round's output into `out`. Weights carry sign as-is: the only caller
  // that ever passes a negative-weight frontier is step()'s linear signed
  // path (recompute()'s seed is always non-negative — see recompute() —
  // and dred() runs its own inline loops, never this one), so loosening
  // the old `w > 0` filter to `w != 0` is a no-op for every pre-existing
  // caller and is what makes retractions propagate here. Returns whether
  // the frontier converged (emptied) before max_iterations_; false means
  // the caller must discard this attempt's admissions and recover.
  bool iterate(DuckDBZSet &frontier, DuckDBZSet &out) {
    size_t iter = 0;
    while (!frontier.empty() && iter++ < max_iterations_) {
      step_view_->apply_changes(sentinel_, frontier);
      DuckDBZSet next;
      for (const auto &[row, w] : step_view_->get_delta()) {
        if (w != 0) {
          admit(row, w, next, out);
        }
      }
      frontier = std::move(next);
    }
    return frontier.empty();
  }

  // Plain multiset weight arithmetic for union_all_: accumulated_/out/
  // frontier all take the signed weight as-is. ZSet::insert erases an
  // entry whose running weight nets to zero, so a row retracted to zero
  // and later re-derived within the same iterate() call correctly nets
  // out to "no change" in `out` — the same net-weight definition
  // recompute()'s diff uses (see recompute()). Set semantics (else branch)
  // is explicitly guarded on w > 0 below — see that branch's comment.
  void admit(const DuckDBRow &row, int64_t w, DuckDBZSet &frontier,
             DuckDBZSet &out) {
    if (union_all_) {
      accumulated_.insert(row, w);
      out.insert(row, w);
      frontier.insert(row, w);
    } else if (w > 0 && accumulated_.get(row) == 0) {
      // Set semantics never admit on a negative w (a retraction reaching
      // here would otherwise be misread as "absent, so admit it"). This
      // branch is never fed negatives today — has_deletion routes
      // non-union_all recursion to dred(), which does not call admit() —
      // but iterate() now passes signed weights through unconditionally
      // for the union_all_ linear path, so guard explicitly rather than
      // rely on that invariant holding forever.
      accumulated_.insert(row, 1);
      out.insert(row, 1);
      frontier.insert(row, 1);
    }
  }

  InputFn anchor_;
  std::unique_ptr<NativeMaterializedView> step_view_;
  std::string sentinel_;
  bool union_all_;
  std::vector<std::pair<std::string, InputFn>> base_inputs_;
  size_t max_iterations_;
  bool linear_step_;
  DuckDBZSet accumulated_;
  DuckDBZSet anchor_total_;                            // ∫ anchor deltas
  std::unordered_map<std::string, DuckDBZSet> base_totals_; // ∫ per table
  DuckDBZSet output_;
  bool has_output_ = false;
};

} // namespace dbsp_native
