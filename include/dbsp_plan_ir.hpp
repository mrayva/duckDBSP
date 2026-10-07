// Planner frontend (Phase B): translate DuckDB logical plans into circuit
// views.
//
// Instead of the bespoke SQL parser, view SQL is parsed/bound/planned by
// DuckDB itself (Connection::ExtractPlan on an internal connection with the
// optimizer disabled, so plan shapes stay canonical: PROJECTION / FILTER /
// GET chains without filter pushdown or projection collapse). The bound
// LogicalOperator tree is walked and mapped onto circuit nodes; bound
// expressions are evaluated row-at-a-time through ExpressionExecutor.
//
// Current scope (B1-B4):
//   B1: LOGICAL_GET -> LOGICAL_FILTER -> LOGICAL_PROJECTION chains
//   B2: LOGICAL_AGGREGATE_AND_GROUP_BY (multi-aggregate, expression keys;
//       HAVING arrives as a FILTER above the aggregate — no special code)
//   B3: LOGICAL_COMPARISON_JOIN (inner equi + residual comparisons),
//       LOGICAL_CROSS_PRODUCT, LOGICAL_DISTINCT, LOGICAL_UNION /
//       LOGICAL_INTERSECT / LOGICAL_EXCEPT (ALL and DISTINCT)
//   B4: LOGICAL_WINDOW (column-ref partitions/orders/args, mapped onto the
//       proven NativeWindowView via EmbeddedViewNode), LOGICAL_MATERIALIZED_CTE
//       + LOGICAL_CTE_REF (definition subtree built once, shared by refs).
//       Correlated subqueries (DELIM_JOIN) and recursive CTEs are rejected
//       with explicit messages.
//   C1: LOGICAL_ORDER_BY / LOGICAL_LIMIT (constant limit/offset only) fold —
//       together with a trailing pure-column-ref projection — into one
//       NativeSortView/NativeLimitView behind an EmbeddedViewNode. When the
//       sort/limit is the plan root, PlannedCircuitView::scan delegates to it
//       so dbsp_query returns rows in ORDER BY order.
//   C2: LOGICAL_RECURSIVE_CTE via PlanRecursiveNode: anchor inline, the
//       recursive step as a nested PlannedCircuitView driven to a fixed
//       point (self-reference = sentinel source). Multi-table recursive
//       steps work; USING KEY is rejected. Insert-only deltas are
//       incremental; deltas containing deletions trigger a full fixed-point
//       recompute from integrated inputs (correct, non-incremental).
//   C3: DISTINCT ON via NativeDistinctOnView in an EmbeddedViewNode
//       (column-ref targets; winner-pick order from the DISTINCT node's own
//       order_by modifier — the ORDER_BY above is presentation, see C1).
//   C4: circuit-IR optimizer (plan_ir::optimize, g_plan_ir_optimize flag):
//       combine adjacent filters, push single-side filters below joins,
//       fuse MAP(FILTER(x)) into one batched node. Successor of the
//       ParsedViewDef-based DBSPOptimizer.
//   D1: vectorized evaluation — filter/map/fused nodes run expressions
//       over shared DataChunk batches (BatchEvaluator); sources/sinks
//       borrow deltas instead of copying.
//   D2: LEFT/RIGHT/FULL outer joins (incrementally reconciled NULL pads).
//   D3: MARK joins (IN/NOT IN, three-valued null-aware marks) and the
//       first() aggregate (uncorrelated scalar subquery comparisons).
// Any other operator yields a DBSP-E110 error naming the operator;
// CDCManager::create_view falls back to the bespoke parser transparently.
//
// Caller contract: PlanTranslator::translate issues queries on an internal
// connection, so the caller must hold an InternalQueryGuard (dbsp_cdc.hpp)
// to keep transaction-commit hooks from recursing into CDCManager.

#pragma once

#include "dbsp_circuit_views.hpp"
#include "dbsp_distinct_on.hpp"
#include "dbsp_errors.hpp"
#include "dbsp_instance_registry.hpp"
#include "dbsp_checkpoint.hpp"
#include "dbsp_qualified_name.hpp"
#include "dbsp_trigger_capability.hpp"
#include "dbsp_flat_packed.hpp"
#include "dbsp_packed_row.hpp"
#include "dbsp_window_view.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/types/vector_cache.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "core_functions/aggregate/quantile_helpers.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_delim_get.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_recursive_cte.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace dbsp_native {


// Keeps the internal connection and the extracted plan alive for as long as
// any node lambda references bound expressions or the client context.
struct PlanKeepAlive {
  std::unique_ptr<duckdb::Connection> connection;
  std::unique_ptr<duckdb::LogicalOperator> plan;
  // Expressions the IR optimizer rewrote (e.g. right-side join pushdown
  // shifts column indices): node lambdas reference them, so they must live
  // exactly as long as the plan itself
  std::vector<std::unique_ptr<duckdb::Expression>> rewritten_exprs;

  PlanKeepAlive() = default;
  PlanKeepAlive(const PlanKeepAlive &) = delete;
  PlanKeepAlive &operator=(const PlanKeepAlive &) = delete;

  ~PlanKeepAlive() {
    if (connection && connection->context) {
      // Destroy the connection first, then unregister: ~Connection fires
      // OnConnectionClosed, which must still classify this context as
      // internal or it would count it as a departing user connection.
      auto *ctx = connection->context.get();
      connection.reset();
      get_instance_registry().unregister_internal(ctx);
    }
  }
};

// Row-at-a-time adapter over ExpressionExecutor: builds a 1-row DataChunk
// from a DuckDBRow, evaluates one bound expression, returns the result Value.
// Correct but slow; vectorized evaluation is a later milestone (B6).
/// DBSP_ROWEVAL_FASTPATH=0 forces every RowExprEval through the generic
/// executor. The fast path must be value-for-value identical to it, so this
/// exists to A/B a suspected divergence (and is what the differential test
/// compares against) -- not as a tuning knob.
inline bool row_eval_fastpath_enabled() {
  static const bool on = [] {
    const char *e = std::getenv("DBSP_ROWEVAL_FASTPATH");
    return e == nullptr || std::string(e) != "0";
  }();
  return on;
}

class RowExprEval {
public:
  RowExprEval(std::shared_ptr<PlanKeepAlive> keep_alive,
              const duckdb::Expression &expr,
              duckdb::vector<duckdb::LogicalType> input_types)
      : keep_alive_(std::move(keep_alive)),
        context_(*keep_alive_->connection->context), expr_(expr),
        input_types_(std::move(input_types)), executor_(context_, expr) {
    if (!input_types_.empty()) {
      chunk_.Initialize(duckdb::Allocator::Get(context_), input_types_);
    }
    // A bare column reference needs no executor at all. The generic path
    // below pays a DataChunk::Reset (a VectorCacheBuffer::ResetFromCache per
    // column), refills EVERY input column, and allocates a fresh result
    // Vector -- all to hand back one column of the row it was given. Join
    // pad reconciliation calls this once per affected row per step, where it
    // profiled as the hottest frame of a cold build.
    if (row_eval_fastpath_enabled() &&
        expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_REF) {
      const auto idx = expr.Cast<duckdb::BoundReferenceExpression>().Index();
      // An index past the input types stays on the generic path rather than
      // being clamped: that case reads a virtual column, and quietly
      // returning a different column is worse than being slow.
      if (idx < input_types_.size()) {
        ref_index_ = idx;
      }
    }
  }

  duckdb::Value eval(const DuckDBRow &row) {
    if (ref_index_ != duckdb::DConstants::INVALID_INDEX) {
      // Must yield exactly what the generic path yields: the missing-column
      // default, the input-type cast the chunk fill applies, and then the
      // expression's own return type (a BOUND_REF's return_type may legally
      // differ from the input type it points at).
      const auto i = ref_index_;
      duckdb::Value v =
          i < row.columns.size() ? row.columns[i] : duckdb::Value(input_types_[i]);
      if (v.type() != input_types_[i]) {
        v = v.DefaultCastAs(input_types_[i]);
      }
      if (v.type() != expr_.GetReturnType()) {
        v = v.DefaultCastAs(expr_.GetReturnType());
      }
      return v;
    }
    chunk_.Reset();
    for (duckdb::idx_t i = 0; i < input_types_.size(); i++) {
      duckdb::Value v = i < row.columns.size()
                            ? row.columns[i]
                            : duckdb::Value(input_types_[i]);
      if (v.type() != input_types_[i]) {
        v = v.DefaultCastAs(input_types_[i]);
      }
      chunk_.SetValue(i, 0, v);
    }
    // DuckDB 2.0: a Vector carries its own size, and DataChunk::SetCardinality
    // sets ONLY the chunk's logical count -- it leaves every child vector at
    // the size Reset() gave it (0). Executors that read the *input vector's*
    // size rather than the passed-down count then see an empty input:
    // VectorOperations::IsNull/IsNotNull (common/vector_operations/
    // null_operations.cpp:17 `auto count = input.size();`) is one, so every
    // `x IS [NOT] NULL` silently evaluated to false for every row.
    // SetChildCardinality sets the child sizes (FlatVector::SetSize, which
    // only stamps the buffer's size -- it does not touch the data we just
    // wrote).
    chunk_.SetChildCardinality(1);
    duckdb::Vector result(expr_.GetReturnType());
    executor_.ExecuteExpression(chunk_, result);
    return result.GetValue(0);
  }

private:
  // Declared first so it outlives the executor during destruction
  std::shared_ptr<PlanKeepAlive> keep_alive_;
  duckdb::ClientContext &context_;
  const duckdb::Expression &expr_;
  duckdb::vector<duckdb::LogicalType> input_types_;
  duckdb::ExpressionExecutor executor_;
  duckdb::DataChunk chunk_;
  /// Column this expression is a bare reference to, or INVALID_INDEX when
  /// the generic executor path is required.
  duckdb::idx_t ref_index_ = duckdb::DConstants::INVALID_INDEX;
};

// Batched adapter over ExpressionExecutor: fills a DataChunk with up to
// STANDARD_VECTOR_SIZE rows and evaluates one bound expression over the
// whole chunk. Amortizes the executor overhead RowExprEval pays per row
// (~4.6x on the filter path, see bench_planner_eval).
class BatchEvaluator {
public:
  BatchEvaluator(std::shared_ptr<PlanKeepAlive> keep_alive,
                 std::vector<const duckdb::Expression *> exprs,
                 duckdb::vector<duckdb::LogicalType> input_types)
      : keep_alive_(std::move(keep_alive)),
        context_(*keep_alive_->connection->context), exprs_(std::move(exprs)),
        input_types_(std::move(input_types)) {
    if (!input_types_.empty()) {
      chunk_.Initialize(duckdb::Allocator::Get(context_), input_types_);
    }
    auto &allocator = duckdb::Allocator::Get(context_);
    for (const auto *expr : exprs_) {
      executors_.push_back(
          std::make_unique<duckdb::ExpressionExecutor>(context_, *expr));
      result_caches_.emplace_back(allocator, expr->GetReturnType());
      results_.emplace_back(result_caches_.back());
    }
  }

  static constexpr duckdb::idx_t kBatch = STANDARD_VECTOR_SIZE;

  size_t expr_count() const { return exprs_.size(); }

  const duckdb::LogicalType &return_type(size_t e) const {
    return exprs_[e]->GetReturnType();
  }

  // Fill the shared input chunk once per batch (count <= kBatch).
  // `col_map`, when given, maps chunk column c to source row column
  // col_map[c] (out-of-range entries fill NULL) — the batched MAP_COLS
  // node projects arbitrary column selections this way.
  void fill(const DuckDBRow *const *rows, duckdb::idx_t count,
            const std::vector<duckdb::idx_t> *col_map = nullptr) {
    chunk_.Reset();
    for (duckdb::idx_t c = 0; c < input_types_.size(); c++) {
      const duckdb::idx_t src = col_map ? (*col_map)[c] : c;
      fill_column(chunk_.data[c], input_types_[c], rows, count, src);
    }
    // DuckDB 2.0: SetCardinality sets only the chunk's logical count; the
    // child vectors keep the size Reset() gave them. Executors that read the
    // input vector's own size (VectorOperations::IsNull/IsNotNull) would see
    // zero rows. See the matching comment in RowExprEval::eval.
    chunk_.SetChildCardinality(count);
    count_ = count;
  }

  // The shared input chunk (valid after fill until the next fill/slice) —
  // the MAP_COLS node hashes its columns directly
  duckdb::DataChunk &input_chunk() { return chunk_; }

  // Restrict the filled chunk to selected rows without refilling
  void slice(duckdb::SelectionVector &sel, duckdb::idx_t count) {
    chunk_.Slice(sel, count);
    count_ = count;
  }

  // Evaluate expression e over the current chunk; result is flattened and
  // valid until the next execute(e) call.
  //
  // The reset-from-cache below is load-bearing correctness, not hygiene:
  // several DuckDB expression executors (CASE's all-one-branch shortcut,
  // and anything else that ends in ExpressionExecutor::Execute of a
  // BOUND_REF root) return by REFERENCING an input chunk column — after
  // such a call results_[e] shares that column's buffer. A later
  // execute(e) on a batch that takes the slow path (e.g. CASE FillSwitch)
  // writes through the vector's existing data pointer, i.e. straight into
  // the shared input chunk, corrupting that column for every expression
  // evaluated after this one in the same batch (found as the
  // self-join+CASE silent-wrong defect; see test_self_join_case.py).
  // Resetting to the slot's own cached buffer first makes stale
  // references impossible while keeping the allocation amortized.
  duckdb::Vector &execute(size_t e) {
    results_[e].ResetFromCache(result_caches_[e]);
    executors_[e]->ExecuteExpression(chunk_, results_[e]);
    results_[e].Flatten(count_);
    return results_[e];
  }

  // The (flattened) result vector of expression e — valid after
  // execute(e) until the next execute(e). Each expression owns its own
  // result storage, so multiple slots stay valid simultaneously (the
  // DP3a output-hash preseed folds across them after all executes).
  duckdb::Vector &result_vector(size_t e) { return results_[e]; }
  duckdb::idx_t current_count() const { return count_; }

  // Read one entry of a flattened evaluation result as a Value, with typed
  // fast paths for common types (Vector::GetValue dispatches per call)
  static duckdb::Value read_result(duckdb::Vector &vec,
                                   const duckdb::LogicalType &type,
                                   duckdb::idx_t i) {
    if (!duckdb::FlatVector::Validity(vec).RowIsValid(i)) {
      return duckdb::Value(type);
    }
    switch (type.id()) {
    case duckdb::LogicalTypeId::BOOLEAN:
      return duckdb::Value::BOOLEAN(
          duckdb::FlatVector::GetData<bool>(vec)[i]);
    case duckdb::LogicalTypeId::INTEGER:
      return duckdb::Value::INTEGER(
          duckdb::FlatVector::GetData<int32_t>(vec)[i]);
    case duckdb::LogicalTypeId::BIGINT:
      return duckdb::Value::BIGINT(
          duckdb::FlatVector::GetData<int64_t>(vec)[i]);
    case duckdb::LogicalTypeId::FLOAT:
      return duckdb::Value::FLOAT(duckdb::FlatVector::GetData<float>(vec)[i]);
    case duckdb::LogicalTypeId::DOUBLE:
      return duckdb::Value::DOUBLE(
          duckdb::FlatVector::GetData<double>(vec)[i]);
    case duckdb::LogicalTypeId::VARCHAR:
      return duckdb::Value(
          duckdb::FlatVector::GetData<duckdb::string_t>(vec)[i].GetString());
    default:
      return vec.GetValue(i);
    }
  }

private:
  // Fill one chunk column from row values. Typed fast paths write vector
  // data directly; anything else falls back to per-value SetValue (which
  // handles casts). Per-cell Value boxing is what made the naive chunk fill
  // barely faster than the row-at-a-time path.
  static void fill_column(duckdb::Vector &vec, const duckdb::LogicalType &type,
                          const DuckDBRow *const *rows, duckdb::idx_t count,
                          duckdb::idx_t c) {
    auto &validity = duckdb::FlatVector::ValidityMutable(vec);
    validity.SetAllValid(count);

    auto value_at = [&](duckdb::idx_t i) -> const duckdb::Value * {
      const auto &cols = rows[i]->columns;
      return c < cols.size() ? &cols[c] : nullptr;
    };
    auto slow_cell = [&](duckdb::idx_t i, const duckdb::Value *v) {
      duckdb::Value cast = v ? *v : duckdb::Value(type);
      if (cast.type() != type) {
        cast = cast.DefaultCastAs(type);
      }
      vec.SetValue(i, cast);
    };

    switch (type.id()) {
    case duckdb::LogicalTypeId::BOOLEAN:
      fill_typed<bool>(vec, validity, type, count, value_at, slow_cell);
      break;
    case duckdb::LogicalTypeId::INTEGER:
      fill_typed<int32_t>(vec, validity, type, count, value_at, slow_cell);
      break;
    case duckdb::LogicalTypeId::BIGINT:
      fill_typed<int64_t>(vec, validity, type, count, value_at, slow_cell);
      break;
    case duckdb::LogicalTypeId::FLOAT:
      fill_typed<float>(vec, validity, type, count, value_at, slow_cell);
      break;
    case duckdb::LogicalTypeId::DOUBLE:
      fill_typed<double>(vec, validity, type, count, value_at, slow_cell);
      break;
    case duckdb::LogicalTypeId::VARCHAR: {
      auto data = duckdb::FlatVector::GetDataMutable<duckdb::string_t>(vec);
      for (duckdb::idx_t i = 0; i < count; i++) {
        const duckdb::Value *v = value_at(i);
        if (!v || v->IsNull()) {
          validity.SetInvalid(i);
        } else if (v->type().id() == duckdb::LogicalTypeId::VARCHAR) {
          data[i] = duckdb::StringVector::AddStringOrBlob(
              vec, duckdb::StringValue::Get(*v));
        } else {
          slow_cell(i, v);
        }
      }
      break;
    }
    default:
      for (duckdb::idx_t i = 0; i < count; i++) {
        slow_cell(i, value_at(i));
      }
      break;
    }
  }

  template <typename T, typename ValueAt, typename SlowCell>
  static void fill_typed(duckdb::Vector &vec, duckdb::ValidityMask &validity,
                         const duckdb::LogicalType &type, duckdb::idx_t count,
                         ValueAt &&value_at, SlowCell &&slow_cell) {
    auto data = duckdb::FlatVector::GetDataMutable<T>(vec);
    for (duckdb::idx_t i = 0; i < count; i++) {
      const duckdb::Value *v = value_at(i);
      if (!v || v->IsNull()) {
        validity.SetInvalid(i);
      } else if (v->type() == type) {
        data[i] = v->GetValueUnsafe<T>();
      } else {
        slow_cell(i, v);
      }
    }
  }

  // Declared first so it outlives the executors during destruction
  std::shared_ptr<PlanKeepAlive> keep_alive_;
  duckdb::ClientContext &context_;
  std::vector<const duckdb::Expression *> exprs_;
  duckdb::vector<duckdb::LogicalType> input_types_;
  std::vector<std::unique_ptr<duckdb::ExpressionExecutor>> executors_;
  // Per-slot owned buffers; execute() resets each result vector from its
  // cache so a result can never retain a stale reference into chunk_
  std::vector<duckdb::VectorCache> result_caches_;
  std::vector<duckdb::Vector> results_;
  duckdb::DataChunk chunk_;
  duckdb::idx_t count_ = 0;
};

// One aggregate within an AGGREGATE spec (B2)
struct PlanAggSpec {
  enum class Fn {
    COUNT_STAR,
    COUNT,
    SUM,
    AVG,
    MIN,
    MAX,
    FIRST,
    STRING_AGG, // order-sensitive: requires ORDER BY inside the aggregate
    ARRAY_AGG,
    MEDIAN,        // quantile_cont 0.5
    QUANTILE_CONT, // interpolated
    QUANTILE_DISC, // lower-bound element
    MODE,          // most frequent (ties break by smallest value)
    MAD            // median absolute deviation (numeric args)
  };

  struct OrderKey {
    const duckdb::Expression *expr = nullptr;
    bool ascending = true;
    bool nulls_first = false;
  };

  Fn fn;
  const duckdb::Expression *arg = nullptr; // null for COUNT_STAR
  const duckdb::Expression *filter = nullptr; // FILTER (WHERE ...) clause
  std::vector<OrderKey> order_keys; // STRING_AGG/ARRAY_AGG
  std::string separator = ",";      // STRING_AGG
  double quantile = 0.5;            // QUANTILE_CONT/DISC
  bool distinct = false;                   // COUNT/SUM/AVG(DISTINCT x)
  bool integer_arg = false;                // SUM/AVG: int64 vs double sum
  bool decimal_arg = false; // SUM over DECIMAL: exact unscaled hugeint sum
  uint8_t decimal_scale = 0;
  duckdb::LogicalType return_type;
};

// One translated plan operator. Forms a tree mirroring the logical plan;
// unary operators have one child, JOIN has two, SET_OP has two or more.
struct PlanOpSpec {
  enum class Kind {
    SOURCE,      // base table scan feeding a SourceNode
    MAP_COLS,    // MapNode selecting columns by index (GET column_ids)
    FILTER_EXPR, // FilterNode over bound predicate expressions (AND)
    MAP_EXPR,    // MapNode evaluating projection expressions
    AGGREGATE,   // PlanAggregateNode (exprs = group keys)
    JOIN,        // PlanJoinNode (inner equi + residual comparisons)
    DISTINCT,    // PlanDistinctNode
    SET_OP,      // PlanSetOpNode
    WINDOW,      // NativeWindowView wrapped in an EmbeddedViewNode
    CTE,         // materialized CTE: children = {definition, main query}
    CTE_REF,     // reads the shared output of a CTE definition
    SORT_LIMIT,  // NativeSortView/NativeLimitView in an EmbeddedViewNode
    REC_CTE,     // WITH RECURSIVE: children = {anchor, recursive step}
    DISTINCT_ON, // NativeDistinctOnView in an EmbeddedViewNode
    FILTER_MAP,  // fused filter+project (IR optimizer, exprs = projection)
    DELIM_JOIN,  // correlated subquery: children = {outer, subplan};
                 // exprs = duplicate-eliminated (correlated) columns
    DELIM_REF    // DELIM_GET: reads the shared distinct-correlated-keys
                 // output of the enclosing DELIM_JOIN
  };

  // Comparison between a left-side and a right-side bound expression
  struct JoinCond {
    const duckdb::Expression *left;
    const duckdb::Expression *right;
    duckdb::ExpressionType cmp;
  };

  enum class SetOp { UNION_ALL, UNION, INTERSECT, INTERSECT_ALL, EXCEPT,
                     EXCEPT_ALL };

  Kind kind;
  std::vector<std::unique_ptr<PlanOpSpec>> children;

  std::string table;                             // SOURCE
  std::vector<duckdb::idx_t> column_idxs;        // MAP_COLS
  std::vector<const duckdb::Expression *> exprs; // FILTER/MAP/group keys
  duckdb::vector<duckdb::LogicalType> input_types; // unary: child's types
  std::vector<PlanAggSpec> agg_specs;            // AGGREGATE
  std::vector<JoinCond> equi_conds;              // JOIN: EQUAL conditions
  std::vector<JoinCond> residual_conds;          // JOIN: other comparisons
  duckdb::vector<duckdb::LogicalType> left_types, right_types; // JOIN
  SetOp set_op = SetOp::UNION_ALL;               // SET_OP
  duckdb::JoinType join_type = duckdb::JoinType::INNER; // JOIN
  bool null_safe_keys = false; // JOIN/DELIM: IS NOT DISTINCT FROM keys
  std::vector<NativeWindowView::WindowDef> window_defs;   // WINDOW
  std::vector<ColumnInfo> window_source_cols;             // WINDOW
  std::vector<ColumnInfo> window_result_cols;             // WINDOW
  duckdb::idx_t cte_index = 0;                   // CTE / CTE_REF

  // DISTINCT_ON: column_idxs = partition keys; sort_columns = winner-pick
  // order from the DISTINCT node's own order_by modifier (not the
  // presentation ORDER_BY above it). Output keeps the full child row layout.
  //
  // SORT_LIMIT: ORDER BY / LIMIT / OFFSET folded into one embedded view.
  // project_idxs is a trailing pure-column-ref projection folded in so sort
  // keys dropped from the output still order it. presentation_root marks the
  // plan root: only then does the view's ordered scan drive dbsp_query.
  std::vector<NativeSortView::SortColumn> sort_columns;
  int64_t limit = -1;        // -1 = no limit
  double limit_percent = -1; // LIMIT p PERCENT (>= 0 wins over limit)
  int64_t offset = 0;
  std::vector<duckdb::idx_t> project_idxs; // empty = identity
  bool presentation_root = false;

  // FILTER_MAP: predicates evaluated before the projection in `exprs`.
  // Pointers reference either the bound plan or PlanKeepAlive::rewritten_exprs
  std::vector<const duckdb::Expression *> filter_exprs;
};

// ===== Circuit-IR optimizer (Phase C4) =====
//
// Rewrites the translated PlanOpSpec tree before circuit construction —
// the successor of the retired ParsedViewDef-based DBSPOptimizer. Passes:
//   1. combine_filters:  FILTER(FILTER(x))      -> one FILTER (AND list)
//   2. pushdown_filters: FILTER above JOIN      -> per-side FILTER below it
//                        (shrinks join index state)
//   3. fuse_filter_map:  MAP(FILTER(x))         -> one FILTER_MAP node
// Projection pruning is deliberately NOT ported: DuckDB's binder already
// prunes via GET column_ids, so canonical plans have nothing left to prune.
inline std::atomic<bool> g_plan_ir_optimize{true};

// L2: intra-operator sharding. When > 1, residual-free inner equi-join
// probe passes over large deltas split across this many threads (probes
// are read-only; each shard emits into its own Z-set, merged after).
// Set by CDCManager::set_parallel_sync — one knob with view-level
// parallelism.
inline std::atomic<int> g_intraop_shards{0};

namespace plan_ir {

inline void collect_bound_refs(const duckdb::Expression &expr,
                               std::vector<duckdb::idx_t> &out) {
  if (expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_REF) {
    out.push_back(expr.Cast<duckdb::BoundReferenceExpression>().Index());
  }
  duckdb::ExpressionIterator::EnumerateChildren(
      expr, [&](const duckdb::Expression &child) {
        collect_bound_refs(child, out);
      });
}

inline void shift_bound_refs(duckdb::Expression &expr, duckdb::idx_t delta) {
  if (expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_REF) {
    expr.Cast<duckdb::BoundReferenceExpression>().IndexMutable() -= delta;
  }
  duckdb::ExpressionIterator::EnumerateChildren(
      expr,
      [&](duckdb::Expression &child) { shift_bound_refs(child, delta); });
}

// FILTER(FILTER(x)) -> FILTER(x) with concatenated AND lists
inline void combine_filters(std::unique_ptr<PlanOpSpec> &spec) {
  while (spec->kind == PlanOpSpec::Kind::FILTER_EXPR &&
         spec->children[0]->kind == PlanOpSpec::Kind::FILTER_EXPR) {
    auto &child = spec->children[0];
    spec->exprs.insert(spec->exprs.end(), child->exprs.begin(),
                       child->exprs.end());
    // Filters preserve schema: the grandchild's output types are the same
    spec->input_types = child->input_types;
    auto grandchild = std::move(child->children[0]);
    spec->children[0] = std::move(grandchild);
  }
}

// FILTER above JOIN: move single-side predicates below the join, shrinking
// the join's per-side index state. Right-side predicates need their column
// indices shifted; the copies live in keep_alive->rewritten_exprs.
inline void pushdown_filters(std::unique_ptr<PlanOpSpec> &spec,
                             PlanKeepAlive &keep_alive) {
  if (spec->kind != PlanOpSpec::Kind::FILTER_EXPR ||
      spec->children[0]->kind != PlanOpSpec::Kind::JOIN) {
    return;
  }
  auto &join = spec->children[0];
  const duckdb::idx_t left_width = join->left_types.size();

  std::vector<const duckdb::Expression *> keep;
  std::vector<const duckdb::Expression *> left_push;
  std::vector<const duckdb::Expression *> right_push;
  for (const auto *expr : spec->exprs) {
    std::vector<duckdb::idx_t> refs;
    collect_bound_refs(*expr, refs);
    bool all_left = true, all_right = true;
    for (auto idx : refs) {
      (idx < left_width ? all_right : all_left) = false;
    }
    if (!refs.empty() && all_left) {
      left_push.push_back(expr); // left indices are already 0-based
    } else if (!refs.empty() && all_right) {
      auto copy = expr->Copy();
      shift_bound_refs(*copy, left_width);
      right_push.push_back(copy.get());
      keep_alive.rewritten_exprs.push_back(std::move(copy));
    } else {
      keep.push_back(expr);
    }
  }
  if (left_push.empty() && right_push.empty()) {
    return;
  }

  auto make_side_filter = [](std::unique_ptr<PlanOpSpec> child,
                             duckdb::vector<duckdb::LogicalType> types,
                             std::vector<const duckdb::Expression *> exprs) {
    auto f = std::make_unique<PlanOpSpec>();
    f->kind = PlanOpSpec::Kind::FILTER_EXPR;
    f->input_types = std::move(types);
    f->exprs = std::move(exprs);
    f->children.push_back(std::move(child));
    return f;
  };
  if (!left_push.empty()) {
    join->children[0] = make_side_filter(
        std::move(join->children[0]), join->left_types, std::move(left_push));
  }
  if (!right_push.empty()) {
    join->children[1] =
        make_side_filter(std::move(join->children[1]), join->right_types,
                         std::move(right_push));
  }

  if (keep.empty()) {
    spec = std::move(spec->children[0]); // filter fully absorbed
  } else {
    spec->exprs = std::move(keep);
  }
}

// MAP(FILTER(x)) -> FILTER_MAP(x): one node, no intermediate Z-set
inline void fuse_filter_map(std::unique_ptr<PlanOpSpec> &spec) {
  if (spec->kind != PlanOpSpec::Kind::MAP_EXPR ||
      spec->children[0]->kind != PlanOpSpec::Kind::FILTER_EXPR) {
    return;
  }
  auto &filter = spec->children[0];
  spec->kind = PlanOpSpec::Kind::FILTER_MAP;
  spec->filter_exprs = std::move(filter->exprs);
  // MAP's input == FILTER's output == FILTER's input (schema-preserving)
  if (!filter->input_types.empty()) {
    spec->input_types = filter->input_types;
  }
  auto grandchild = std::move(filter->children[0]);
  spec->children[0] = std::move(grandchild);
}

// Clone a bound expression and remap every BOUND_REF leaf through the
// MAP_COLS column selection. Returns nullptr when any referenced column
// cannot be remapped (out-of-range / virtual-rowid entries).
inline duckdb::unique_ptr<duckdb::Expression>
remap_bound_refs(const duckdb::Expression &expr,
                 const std::vector<duckdb::idx_t> &idxs,
                 duckdb::idx_t source_arity) {
  auto clone = expr.Copy();
  bool ok = true;
  std::function<void(duckdb::Expression &)> walk =
      [&](duckdb::Expression &e) {
        if (e.GetExpressionClass() == duckdb::ExpressionClass::BOUND_REF) {
          auto &ref = e.Cast<duckdb::BoundReferenceExpression>();
          if (ref.Index() >= idxs.size() || idxs[ref.Index()] >= source_arity) {
            ok = false;
            return;
          }
          ref.IndexMutable() = idxs[ref.Index()];
          return;
        }
        duckdb::ExpressionIterator::EnumerateChildren(
            e, [&](duckdb::Expression &child) { walk(child); });
      };
  walk(*clone);
  return ok ? std::move(clone) : nullptr;
}

// FILTER_MAP/MAP_EXPR over MAP_COLS(x): remap the consumer's bound refs
// through the column selection and drop the MAP_COLS node. MAP_COLS
// re-materializes every input row (per-Value copies + a lazily hashed
// output Z-set insert) — measured as the DOMINANT cost of simple
// filter/projection views (~72ms of a 90ms 100k-row sync), so eliding it
// matters more than batching it.
//
// Deliberately EXCLUDES bare FILTER_EXPR: unlike MAP_EXPR/FILTER_MAP,
// whose `exprs` fully define their own output columns, FILTER_EXPR is a
// pure passthrough — its output row layout IS its input's layout, not
// something remap_bound_refs fixes up. Eliding MAP_COLS beneath a bare
// FILTER_EXPR would correctly remap the filter's own predicate but
// silently change the column order/width it hands to its PARENT (whoever
// that is — e.g. an AGGREGATE reading group keys / agg args by position
// right above a `WHERE ... GROUP BY` filter), corrupting any ancestor
// still indexed against the old MAP_COLS-selected layout. A bare
// FILTER_EXPR only remains after fuse_filter_map when its own parent is
// NOT a MAP_EXPR (that pairing already fused into a self-defining
// FILTER_MAP above), so there is no self-defining consumer to safely
// absorb the reorder into.
inline void fuse_map_cols(std::unique_ptr<PlanOpSpec> &spec,
                          PlanKeepAlive &keep_alive) {
  if (spec->kind != PlanOpSpec::Kind::FILTER_MAP &&
      spec->kind != PlanOpSpec::Kind::MAP_EXPR) {
    return;
  }
  if (spec->children.size() != 1 ||
      spec->children[0]->kind != PlanOpSpec::Kind::MAP_COLS) {
    return;
  }
  auto &map_cols = spec->children[0];
  if (map_cols->input_types.empty()) {
    return; // legacy spec without source layout: leave as-is
  }
  const auto &idxs = map_cols->column_idxs;
  const auto arity = map_cols->input_types.size();

  // clone+remap everything first; bail wholesale on any failure
  std::vector<duckdb::unique_ptr<duckdb::Expression>> remapped;
  std::vector<const duckdb::Expression *> new_filters, new_exprs;
  for (const auto *e : spec->filter_exprs) {
    auto r = remap_bound_refs(*e, idxs, arity);
    if (!r) {
      return;
    }
    new_filters.push_back(r.get());
    remapped.push_back(std::move(r));
  }
  for (const auto *e : spec->exprs) {
    auto r = remap_bound_refs(*e, idxs, arity);
    if (!r) {
      return;
    }
    new_exprs.push_back(r.get());
    remapped.push_back(std::move(r));
  }

  for (auto &r : remapped) {
    keep_alive.rewritten_exprs.push_back(std::move(r));
  }
  spec->filter_exprs = std::move(new_filters);
  spec->exprs = std::move(new_exprs);
  spec->input_types = map_cols->input_types; // chunk layout = full CDC row
  spec->children[0] = std::move(map_cols->children[0]);
}

inline void optimize(std::unique_ptr<PlanOpSpec> &spec,
                     PlanKeepAlive &keep_alive) {
  combine_filters(spec);
  pushdown_filters(spec, keep_alive);
  fuse_filter_map(spec);
  fuse_map_cols(spec, keep_alive);
  for (auto &child : spec->children) {
    optimize(child, keep_alive);
  }
}

} // namespace plan_ir

} // namespace dbsp_native
