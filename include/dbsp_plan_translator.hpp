#pragma once

#include "dbsp_planned_circuit_view.hpp"

namespace dbsp_native {
// A bind failure on the plan-extraction connection has one cause that the
// engine's own wording sends the reader looking in the wrong place: the source
// table exists, but only inside the CALLER's uncommitted transaction, and the
// internal connection the plan is extracted on cannot see it. That is what
//   BEGIN; CREATE TABLE t (...);
//   CREATE MATERIALIZED VIEW tv AS SELECT SUM(v) FROM t;
// produces, as `Table with name t does not exist! Did you mean "g1.t"?` —
// which reads like a typo. Failing here is the right outcome (loud, and the
// same every time); only the explanation improves. The test is the one the
// message claims: the name resolves on the caller's own context, so it is a
// visibility problem and not a missing table.
inline std::string
explain_bind_error(duckdb::ClientContext &context, const std::string &what) {
  const std::string prefix = "planner frontend: ";
  const std::string marker = "Table with name ";
  const auto pos = what.find(marker);
  if (pos == std::string::npos || !user_transaction_open(context)) {
    return prefix + what;
  }
  const auto start = pos + marker.size();
  const auto end = what.find(" does not exist", start);
  if (end == std::string::npos) {
    return prefix + what;
  }
  const std::string name = what.substr(start, end - start);
  if (!resolve_table_entry(context, name)) {
    return prefix + what; // genuinely missing: the engine's message is right
  }
  return prefix + "source table '" + name +
         "' is not yet committed — an open transaction's CREATE TABLE is "
         "invisible to the connection that plans the view. COMMIT the CREATE "
         "TABLE before creating a materialized view over it. (" +
         what + ")";
}

class PlanTranslator {
public:
  struct Result {
    std::unique_ptr<NativeMaterializedView> view;
    std::string error; // set when view is null
  };

  // Translate view SQL into a circuit view via DuckDB's planner.
  // Caller must hold an InternalQueryGuard.
  //
  // mv_schemas: name + schema of every existing materialized view. MVs are
  // not in DuckDB's catalog, so views-on-views would fail to bind; empty
  // TEMP tables mirroring their schemas are created on the internal
  // connection (the temp schema shadows main, matching CDC's own
  // views-before-tables resolution order). They exist only for plan
  // extraction — no data ever flows through them.
  static Result translate(
      duckdb::ClientContext &context, const std::string &view_name,
      const std::string &sql,
      const std::vector<std::pair<std::string, TableSchema>> &mv_schemas = {}) {
    auto keep_alive = std::make_shared<PlanKeepAlive>();
    try {
      auto &db = duckdb::DatabaseInstance::GetDatabase(context);
      keep_alive->connection = std::make_unique<duckdb::Connection>(db);
      get_instance_registry().register_internal(
          keep_alive->connection->context.get(), &db);
      for (const auto &[mv_name, mv_schema] : mv_schemas) {
        std::string ddl = "CREATE TEMP TABLE \"" + mv_name + "\" (";
        for (size_t i = 0; i < mv_schema.columns.size(); i++) {
          if (i > 0) {
            ddl += ", ";
          }
          ddl += "\"" + mv_schema.columns[i].name + "\" " +
                 mv_schema.columns[i].type.ToString();
        }
        ddl += ")";
        auto res = keep_alive->connection->Query(ddl);
        if (res->HasError()) {
          return {nullptr, "planner frontend: could not shadow view '" +
                               mv_name + "': " + res->GetError()};
        }
      }
      // Canonical plan shapes: no filter pushdown into GET, no projection
      // collapse. ExtractPlan still runs ColumnBindingResolver and
      // ResolveOperatorTypes.
      duckdb::Settings::Set<duckdb::EnableOptimizerSetting>(
          *keep_alive->connection->context, duckdb::SetScope::SESSION,
          duckdb::Value::BOOLEAN(false));
      keep_alive->plan = keep_alive->connection->ExtractPlan(sql);
    } catch (const std::exception &e) {
      return {nullptr, explain_bind_error(context, e.what())};
    }

    Walker walker;
    auto root = walker.visit(*keep_alive->plan);
    if (!root) {
      return {nullptr, walker.error};
    }
    for (auto &e : walker.owned_exprs) {
      keep_alive->rewritten_exprs.push_back(std::move(e));
    }
    walker.owned_exprs.clear();
    if (root->kind == PlanOpSpec::Kind::SORT_LIMIT) {
      // Only a root sort/limit drives dbsp_query's scan order; nested ones
      // (subqueries) affect membership only
      root->presentation_root = true;
    }
    if (g_plan_ir_optimize) {
      plan_ir::optimize(root, *keep_alive);
    }

    TableSchema schema;
    schema.table_name = view_name;
    schema.columns = walker.columns;
    // The walker names columns from plan expressions; bound references
    // (e.g. GROUP BY 1,2 output) have no alias and degrade to "0","1".
    // The binder's real output names live on the prepared statement, so
    // prefer those when they line up.
    try {
      auto prep = keep_alive->connection->Prepare(sql);
      if (prep && !prep->HasError()) {
        const auto &names = prep->GetNames();
        if (names.size() == schema.columns.size()) {
          for (size_t i = 0; i < names.size(); i++) {
            schema.columns[i].name = names[i].GetIdentifierName();
          }
        }
      }
    } catch (...) {
      // keep walker-derived names
    }
    // Deduplicate column names (e.g. t.val and u.val in a join): repeated
    // names would make the result unqueryable through dbsp_query
    std::unordered_map<std::string, int> seen;
    for (auto &col : schema.columns) {
      int &n = seen[col.name];
      if (n++ > 0) {
        col.name += "_" + std::to_string(n - 1);
      }
    }

    auto view = std::make_unique<PlannedCircuitView>(
        view_name, sql, schema, std::move(keep_alive), *root);
    return {std::move(view), ""};
  }

private:
  struct Walker {
    std::vector<ColumnInfo> columns; // schema of current operator's output
    std::string error;
    // Expressions synthesized during translation (NULL pads / GROUPING
    // constants for grouping-set branches). Moved into
    // PlanKeepAlive::rewritten_exprs after the walk so they outlive the
    // circuit's evaluators.
    std::vector<std::unique_ptr<duckdb::Expression>> owned_exprs;
    duckdb::idx_t synthetic_cte_seq_ = 0; // grouping-set input sharing

    using SpecPtr = std::unique_ptr<PlanOpSpec>;

    SpecPtr unsupported(const std::string &what) {
      error = format_error_code(ErrorCode::PLAN_OPERATOR_NOT_SUPPORTED) +
              ": unsupported in planner frontend: " + what;
      return nullptr;
    }

    SpecPtr visit(duckdb::LogicalOperator &op) {
      switch (op.type) {
      case duckdb::LogicalOperatorType::LOGICAL_PROJECTION:
        return visit_projection(op.Cast<duckdb::LogicalProjection>());
      case duckdb::LogicalOperatorType::LOGICAL_FILTER:
        return visit_filter(op.Cast<duckdb::LogicalFilter>());
      case duckdb::LogicalOperatorType::LOGICAL_GET:
        return visit_get(op.Cast<duckdb::LogicalGet>());
      case duckdb::LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
        return visit_aggregate(op.Cast<duckdb::LogicalAggregate>());
      case duckdb::LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
        return visit_join(op.Cast<duckdb::LogicalComparisonJoin>());
      case duckdb::LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
        return visit_cross_product(op);
      case duckdb::LogicalOperatorType::LOGICAL_DISTINCT:
        return visit_distinct(op.Cast<duckdb::LogicalDistinct>());
      case duckdb::LogicalOperatorType::LOGICAL_UNION:
      case duckdb::LogicalOperatorType::LOGICAL_INTERSECT:
      case duckdb::LogicalOperatorType::LOGICAL_EXCEPT:
        return visit_set_operation(op.Cast<duckdb::LogicalSetOperation>());
      case duckdb::LogicalOperatorType::LOGICAL_WINDOW:
        return visit_window(op.Cast<duckdb::LogicalWindow>());
      case duckdb::LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
        return visit_cte(op.Cast<duckdb::LogicalMaterializedCTE>());
      case duckdb::LogicalOperatorType::LOGICAL_CTE_REF:
        return visit_cte_ref(op.Cast<duckdb::LogicalCTERef>());
      case duckdb::LogicalOperatorType::LOGICAL_ORDER_BY:
        return visit_order(op.Cast<duckdb::LogicalOrder>(), /*limit=*/-1,
                           /*offset=*/0);
      case duckdb::LogicalOperatorType::LOGICAL_LIMIT:
        return visit_limit(op.Cast<duckdb::LogicalLimit>());
      case duckdb::LogicalOperatorType::LOGICAL_DELIM_JOIN:
        return visit_delim_join(op.Cast<duckdb::LogicalComparisonJoin>());
      case duckdb::LogicalOperatorType::LOGICAL_DELIM_GET:
        return visit_delim_get(op.Cast<duckdb::LogicalDelimGet>());
      case duckdb::LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
        return visit_recursive_cte(op.Cast<duckdb::LogicalRecursiveCTE>());
      default:
        return unsupported("logical operator " + op.GetName());
      }
    }

    SpecPtr visit_projection(duckdb::LogicalProjection &op) {
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }
      // A pure column-ref projection directly above ORDER BY/LIMIT folds into
      // the sort/limit view as its ProjectFn: sort keys dropped from the
      // SELECT list still order the output (the view sorts full input rows)
      if (child->kind == PlanOpSpec::Kind::SORT_LIMIT &&
          child->project_idxs.empty()) {
        bool pure_refs = true;
        for (const auto &expr : op.expressions) {
          if (expr->GetExpressionClass() !=
              duckdb::ExpressionClass::BOUND_REF) {
            pure_refs = false;
            break;
          }
        }
        if (pure_refs) {
          for (const auto &expr : op.expressions) {
            child->project_idxs.push_back(
                expr->Cast<duckdb::BoundReferenceExpression>().Index());
          }
          columns.clear();
          for (duckdb::idx_t i = 0; i < op.expressions.size(); i++) {
            columns.push_back({op.expressions[i]->GetName().GetIdentifierName(), op.types[i]});
          }
          return child;
        }
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::MAP_EXPR;
      spec->input_types = op.children[0]->types;
      for (const auto &expr : op.expressions) {
        spec->exprs.push_back(expr.get());
      }
      spec->children.push_back(std::move(child));

      columns.clear();
      for (duckdb::idx_t i = 0; i < op.expressions.size(); i++) {
        columns.push_back({op.expressions[i]->GetName().GetIdentifierName(), op.types[i]});
      }
      return spec;
    }

    SpecPtr visit_filter(duckdb::LogicalFilter &op) {
      if (!op.projection_map.empty()) {
        return unsupported("FILTER with projection map");
      }
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::FILTER_EXPR;
      spec->input_types = op.children[0]->types;
      for (const auto &expr : op.expressions) {
        spec->exprs.push_back(expr.get());
      }
      spec->children.push_back(std::move(child));
      // Filter passes rows through unchanged; columns stay as-is
      return spec;
    }

    SpecPtr visit_limit(duckdb::LogicalLimit &op) {
      int64_t limit = -1, offset = 0;
      double limit_percent = -1;
      using LT = duckdb::LimitNodeType;
      if (op.limit_val.Type() == LT::CONSTANT_VALUE) {
        limit = static_cast<int64_t>(op.limit_val.GetConstantValue());
      } else if (op.limit_val.Type() == LT::CONSTANT_PERCENTAGE) {
        limit_percent = op.limit_val.GetConstantPercentage();
      } else if (op.limit_val.Type() != LT::UNSET) {
        return unsupported("non-constant LIMIT");
      }
      if (op.offset_val.Type() == LT::CONSTANT_VALUE) {
        offset = static_cast<int64_t>(op.offset_val.GetConstantValue());
      } else if (op.offset_val.Type() != LT::UNSET) {
        return unsupported("non-constant OFFSET");
      }
      auto &child = *op.children[0];
      if (child.type == duckdb::LogicalOperatorType::LOGICAL_ORDER_BY) {
        return visit_order(child.Cast<duckdb::LogicalOrder>(), limit, offset,
                           limit_percent);
      }
      auto child_spec = visit(child);
      if (!child_spec) {
        return nullptr;
      }
      return make_sort_limit(std::move(child_spec), {}, limit, offset,
                             limit_percent);
    }

    SpecPtr visit_order(duckdb::LogicalOrder &op, int64_t limit,
                        int64_t offset, double limit_percent = -1) {
      if (!op.projection_map.empty()) {
        return unsupported("ORDER BY with projection map");
      }
      std::vector<NativeSortView::SortColumn> cols;
      for (auto &o : op.orders) {
        if (o.expression->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_REF) {
          return unsupported("ORDER BY expression (use a plain column)");
        }
        auto &ref = o.expression->Cast<duckdb::BoundReferenceExpression>();
        NativeSortView::SortColumn sc;
        sc.column_idx = static_cast<size_t>(ref.Index());
        sc.ascending = o.type != duckdb::OrderType::DESCENDING;
        sc.nulls_first = o.null_order == duckdb::OrderByNullType::NULLS_FIRST;
        cols.push_back(sc);
      }
      auto child_spec = visit(*op.children[0]);
      if (!child_spec) {
        return nullptr;
      }
      return make_sort_limit(std::move(child_spec), std::move(cols), limit,
                             offset, limit_percent);
    }

    // ORDER BY/LIMIT pass rows through (LIMIT changes membership, not
    // layout): columns stay as the child left them
    SpecPtr make_sort_limit(SpecPtr child,
                            std::vector<NativeSortView::SortColumn> cols,
                            int64_t limit, int64_t offset,
                            double limit_percent = -1) {
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::SORT_LIMIT;
      spec->sort_columns = std::move(cols);
      spec->limit = limit;
      spec->offset = offset;
      spec->limit_percent = limit_percent;
      spec->children.push_back(std::move(child));
      return spec;
    }

    SpecPtr visit_aggregate(duckdb::LogicalAggregate &op) {
      // Parse aggregate expressions once; grouping-set branches reuse them
      std::vector<PlanAggSpec> parsed;
      if (!parse_agg_exprs(op, parsed)) {
        return nullptr;
      }
      if (op.grouping_sets.size() > 1 || !op.grouping_functions.empty()) {
        return build_grouping_sets(op, parsed);
      }
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }

      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::AGGREGATE;
      spec->input_types = op.children[0]->types;
      for (const auto &group : op.groups) {
        spec->exprs.push_back(group.get());
      }
      spec->agg_specs = std::move(parsed);
      spec->children.push_back(std::move(child));

      // Output layout: group values first, then aggregate values
      columns.clear();
      for (duckdb::idx_t i = 0; i < op.groups.size(); i++) {
        columns.push_back({op.groups[i]->GetName().GetIdentifierName(), op.types[i]});
      }
      for (duckdb::idx_t i = 0; i < op.expressions.size(); i++) {
        columns.push_back(
            {op.expressions[i]->GetName().GetIdentifierName(), op.types[op.groups.size() + i]});
      }
      return spec;
    }

    // GROUPING SETS / ROLLUP / CUBE: one aggregate branch per grouping
    // set over its own copy of the input subtree, each mapped to the full
    // output layout (excluded group columns → typed NULLs, GROUPING()
    // → per-branch constant bitmask), then UNION ALL. Each branch is an
    // ordinary incremental aggregate, so the whole construct is
    // incremental for free.
    SpecPtr build_grouping_sets(duckdb::LogicalAggregate &op,
                                const std::vector<PlanAggSpec> &parsed) {
      const size_t num_groups = op.groups.size();
      const size_t num_aggs = op.expressions.size();

      std::vector<duckdb::GroupingSet> sets = op.grouping_sets;
      if (sets.empty()) {
        duckdb::GroupingSet all;
        for (duckdb::idx_t j = 0; j < num_groups; j++) {
          all.insert(duckdb::ProjectionIndex(j));
        }
        sets.push_back(std::move(all));
      }

      auto union_spec = std::make_unique<PlanOpSpec>();
      union_spec->kind = PlanOpSpec::Kind::SET_OP;
      union_spec->set_op = PlanOpSpec::SetOp::UNION_ALL;

      // Build the input subtree ONCE and share it across all grouping-set
      // branches through the CTE machinery (a synthetic index well above
      // DuckDB's table_index range) — CUBE(3) = 8 branches would
      // otherwise recompute the input 8 times per delta
      auto shared_input = visit(*op.children[0]);
      if (!shared_input) {
        return nullptr;
      }
      const duckdb::idx_t synthetic_cte =
          (duckdb::idx_t(1) << 40) + synthetic_cte_seq_++;

      for (const auto &gset : sets) {
        auto child = std::make_unique<PlanOpSpec>();
        child->kind = PlanOpSpec::Kind::CTE_REF;
        child->cte_index = synthetic_cte;

        auto agg = std::make_unique<PlanOpSpec>();
        agg->kind = PlanOpSpec::Kind::AGGREGATE;
        agg->input_types = op.children[0]->types;
        std::unordered_map<duckdb::idx_t, size_t> pos_in_set;
        duckdb::vector<duckdb::LogicalType> agg_out_types;
        for (duckdb::idx_t j : gset) { // set<idx_t>: ascending
          pos_in_set[j] = agg->exprs.size();
          agg->exprs.push_back(op.groups[j].get());
          agg_out_types.push_back(op.types[j]);
        }
        agg->agg_specs = parsed;
        agg->children.push_back(std::move(child));
        for (size_t k = 0; k < num_aggs; k++) {
          agg_out_types.push_back(op.types[num_groups + k]);
        }

        // Map the branch to the full layout
        auto map = std::make_unique<PlanOpSpec>();
        map->kind = PlanOpSpec::Kind::MAP_EXPR;
        map->input_types = agg_out_types;
        for (duckdb::idx_t j = 0; j < num_groups; j++) {
          std::unique_ptr<duckdb::Expression> e;
          auto it = pos_in_set.find(j);
          if (it != pos_in_set.end()) {
            e = duckdb::make_uniq<duckdb::BoundReferenceExpression>(
                op.types[j], it->second);
          } else {
            e = duckdb::make_uniq<duckdb::BoundConstantExpression>(
                duckdb::Value(op.types[j]));
          }
          map->exprs.push_back(e.get());
          owned_exprs.push_back(std::move(e));
        }
        for (size_t k = 0; k < num_aggs; k++) {
          auto e = duckdb::make_uniq<duckdb::BoundReferenceExpression>(
              op.types[num_groups + k], gset.size() + k);
          map->exprs.push_back(e.get());
          owned_exprs.push_back(std::move(e));
        }
        for (size_t f = 0; f < op.grouping_functions.size(); f++) {
          // GROUPING(c1..cn): bit i (MSB-first) set when ci is NOT part
          // of this branch's grouping set
          const auto &args = op.grouping_functions[f];
          int64_t mask = 0;
          for (size_t a = 0; a < args.size(); a++) {
            mask <<= 1;
            if (!gset.count(args[a])) {
              mask |= 1;
            }
          }
          auto e = duckdb::make_uniq<duckdb::BoundConstantExpression>(
              duckdb::Value::BIGINT(mask));
          map->exprs.push_back(e.get());
          owned_exprs.push_back(std::move(e));
        }
        map->children.push_back(std::move(agg));
        union_spec->children.push_back(std::move(map));
      }

      columns.clear();
      for (duckdb::idx_t j = 0; j < num_groups; j++) {
        columns.push_back({op.groups[j]->GetName().GetIdentifierName(), op.types[j]});
      }
      for (size_t k = 0; k < num_aggs; k++) {
        columns.push_back(
            {op.expressions[k]->GetName().GetIdentifierName(), op.types[num_groups + k]});
      }
      for (size_t f = 0; f < op.grouping_functions.size(); f++) {
        columns.push_back({"grouping_" + std::to_string(f),
                           op.types[num_groups + num_aggs + f]});
      }

      SpecPtr body = union_spec->children.size() == 1
                         ? std::move(union_spec->children[0])
                         : std::move(union_spec);
      auto cte = std::make_unique<PlanOpSpec>();
      cte->kind = PlanOpSpec::Kind::CTE;
      cte->cte_index = synthetic_cte;
      cte->children.push_back(std::move(shared_input));
      cte->children.push_back(std::move(body));
      return cte;
    }

    // Parse BoundAggregateExpressions into PlanAggSpecs (fn, argument,
    // FILTER predicate, DISTINCT). Returns false with error set on
    // unsupported constructs.
    bool parse_agg_exprs(duckdb::LogicalAggregate &op,
                         std::vector<PlanAggSpec> &out) {
      for (const auto &expr : op.expressions) {
        if (expr->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_AGGREGATE) {
          unsupported("non-aggregate expression in AGGREGATE");
          return false;
        }
        auto &agg = expr->Cast<duckdb::BoundAggregateExpression>();

        PlanAggSpec agg_spec;
        agg_spec.distinct = agg.IsDistinct();
        agg_spec.filter = agg.GetFilter().get();
        agg_spec.return_type = agg.GetReturnType();
        const std::string &fn = agg.Function().GetName().GetIdentifierName();
        if (fn == "count_star") {
          agg_spec.fn = PlanAggSpec::Fn::COUNT_STAR;
        } else if (fn == "count") {
          agg_spec.fn = PlanAggSpec::Fn::COUNT;
        } else if (fn == "sum" || fn == "sum_no_overflow") {
          agg_spec.fn = PlanAggSpec::Fn::SUM;
        } else if (fn == "avg") {
          agg_spec.fn = PlanAggSpec::Fn::AVG;
        } else if (fn == "min") {
          agg_spec.fn = PlanAggSpec::Fn::MIN;
        } else if (fn == "max") {
          agg_spec.fn = PlanAggSpec::Fn::MAX;
        } else if (fn == "first" || fn == "arbitrary") {
          // Scalar-subquery plans wrap the inner aggregate in first();
          // the input there is a single row, so any deterministic pick
          // (smallest value) is exact
          agg_spec.fn = PlanAggSpec::Fn::FIRST;
        } else if (fn == "string_agg" || fn == "group_concat" ||
                   fn == "listagg") {
          agg_spec.fn = PlanAggSpec::Fn::STRING_AGG;
        } else if (fn == "array_agg" || fn == "list") {
          agg_spec.fn = PlanAggSpec::Fn::ARRAY_AGG;
        } else if (fn == "median") {
          agg_spec.fn = PlanAggSpec::Fn::MEDIAN;
        } else if (fn == "quantile_cont" || fn == "quantile_disc" ||
                   fn == "quantile") {
          agg_spec.fn = fn == "quantile_cont"
                            ? PlanAggSpec::Fn::QUANTILE_CONT
                            : PlanAggSpec::Fn::QUANTILE_DISC;
          // The fraction argument is erased at bind time and stored in
          // QuantileBindData (public core_functions header — no layout
          // mirror needed, unlike string_agg's separator)
          if (!agg.BindInfo()) {
            unsupported(fn + " without bind data");
            return false;
          }
          const auto &qbd =
              agg.BindInfo()->Cast<duckdb::QuantileBindData>();
          if (qbd.quantiles.size() != 1) {
            unsupported(fn + " with a fraction LIST (single fraction only)");
            return false;
          }
          agg_spec.quantile = qbd.quantiles[0].dbl;
        } else if (fn == "mode") {
          agg_spec.fn = PlanAggSpec::Fn::MODE;
        } else if (fn == "mad") {
          agg_spec.fn = PlanAggSpec::Fn::MAD;
          // Temporal mad (DATE/TIMESTAMP → INTERVAL) needs interval
          // arithmetic we don't do — numeric only
          if (!agg.GetChildren().empty() &&
              !agg.GetChildren()[0]->GetReturnType().IsNumeric()) {
            unsupported("mad over " +
                        agg.GetChildren()[0]->GetReturnType().ToString());
            return false;
          }
        } else {
          unsupported("aggregate function " + fn);
          return false;
        }
        if ((agg_spec.fn == PlanAggSpec::Fn::MEDIAN ||
             agg_spec.fn == PlanAggSpec::Fn::QUANTILE_CONT ||
             agg_spec.fn == PlanAggSpec::Fn::QUANTILE_DISC ||
             agg_spec.fn == PlanAggSpec::Fn::MODE ||
             agg_spec.fn == PlanAggSpec::Fn::MAD) &&
            agg_spec.distinct) {
          unsupported("DISTINCT on " + fn);
          return false;
        }
        if (agg_spec.fn == PlanAggSpec::Fn::FIRST &&
            (agg_spec.distinct || agg.GetOrderBys())) {
          // first() IS order/multiplicity sensitive — modifiers would
          // change its meaning
          unsupported("DISTINCT/ORDER BY on " + fn);
          return false;
        }
        // ORDER BY inside COUNT/SUM/AVG/MIN/MAX is semantically inert
        // (order-insensitive aggregates) — accept and ignore

        if (agg_spec.fn == PlanAggSpec::Fn::STRING_AGG ||
            agg_spec.fn == PlanAggSpec::Fn::ARRAY_AGG) {
          // Without an internal ORDER BY the result order is whatever
          // DuckDB's scan produced — unreproducible incrementally after
          // deletes/reinserts. Deterministic subset only.
          if (!agg.GetOrderBys() || agg.GetOrderBys()->orders.empty()) {
            unsupported(fn + " without ORDER BY inside the aggregate "
                             "(add e.g. " + fn + "(x ORDER BY x))");
            return false;
          }
          if (agg_spec.distinct) {
            unsupported("DISTINCT on " + fn);
            return false;
          }
          for (const auto &o : agg.GetOrderBys()->orders) {
            PlanAggSpec::OrderKey key;
            key.expr = o.expression.get();
            key.ascending = o.type != duckdb::OrderType::DESCENDING;
            key.nulls_first =
                o.null_order == duckdb::OrderByNullType::NULLS_FIRST;
            agg_spec.order_keys.push_back(key);
          }
          if (agg_spec.fn == PlanAggSpec::Fn::STRING_AGG &&
              agg.BindInfo()) {
            // DuckDB's bind erases the separator argument and stores it in
            // a StringAggBindData that is private to its own translation
            // unit (extension/core_functions/aggregate/distributive/
            // string_agg.cpp), so there is no supported way to read it.
            // This mirrors that struct's LAYOUT: `string sep` as the sole
            // member after the FunctionData base.
            //
            // UNSAFE BY CONSTRUCTION, kept only because it is verified.
            // The engine is pinned in-tree (currently v2.0.0-alpha39998),
            // and the layout was re-checked against 2.0's source and
            // re-verified empirically by the differential sweep
            // (`string_agg(v, '-' ORDER BY v)` byte-identical to plain
            // SQL). Neither of those is a guarantee: nothing here fails to
            // compile if upstream adds a member or reorders it — it would
            // read the wrong bytes and produce a wrong separator. RE-VERIFY
            // ON EVERY ENGINE BUMP, and prefer a supported accessor if one
            // ever appears.
            struct SeparatorMirror : public duckdb::FunctionData {
              std::string sep;
              duckdb::unique_ptr<duckdb::FunctionData>
              Copy() const override {
                return nullptr;
              }
              bool Equals(const duckdb::FunctionData &) const override {
                return false;
              }
            };
            agg_spec.separator =
                reinterpret_cast<const SeparatorMirror *>(
                    agg.BindInfo().get())
                    ->sep;
          }
          agg_spec.arg = agg.GetChildren()[0].get();
          out.push_back(std::move(agg_spec));
          continue;
        }

        if (agg_spec.fn != PlanAggSpec::Fn::COUNT_STAR) {
          if (agg.GetChildren().size() != 1) {
            unsupported("aggregate with " +
                        std::to_string(agg.GetChildren().size()) +
                        " arguments (" + fn + ")");
            return false;
          }
          agg_spec.arg = agg.GetChildren()[0].get();
          if (agg_spec.fn == PlanAggSpec::Fn::SUM ||
              agg_spec.fn == PlanAggSpec::Fn::AVG) {
            switch (agg_spec.arg->GetReturnType().id()) {
            case duckdb::LogicalTypeId::TINYINT:
            case duckdb::LogicalTypeId::SMALLINT:
            case duckdb::LogicalTypeId::INTEGER:
            case duckdb::LogicalTypeId::BIGINT:
            case duckdb::LogicalTypeId::UTINYINT:
            case duckdb::LogicalTypeId::USMALLINT:
            case duckdb::LogicalTypeId::UINTEGER:
              agg_spec.integer_arg = true;
              break;
            case duckdb::LogicalTypeId::FLOAT:
            case duckdb::LogicalTypeId::DOUBLE:
              agg_spec.integer_arg = false;
              break;
            case duckdb::LogicalTypeId::DECIMAL:
              if (agg_spec.fn == PlanAggSpec::Fn::SUM) {
                // SUM(DECIMAL) stays exact; AVG(DECIMAL) returns DOUBLE in
                // DuckDB, so the double path matches its semantics
                agg_spec.decimal_arg = true;
                agg_spec.decimal_scale =
                    duckdb::DecimalType::GetScale(agg_spec.arg->GetReturnType());
              } else {
                agg_spec.integer_arg = false;
              }
              break;
            default:
              unsupported(fn + " over " +
                          agg_spec.arg->GetReturnType().ToString());
              return false;
            }
          }
        }
        out.push_back(std::move(agg_spec));
      }
      return true;
    }

    // Correlated subqueries. DELIM_JOIN evaluates its right subplan with
    // DELIM_GET = the DISTINCT correlated-key values of the left side,
    // then joins back with null-safe (IS NOT DISTINCT FROM) conditions.
    // SINGLE (scalar subquery, inner is grouped by the key so at most one
    // match) maps onto the LEFT-join padding machinery; MARK (EXISTS) onto
    // the mark machinery.
    std::vector<std::vector<ColumnInfo>> delim_columns_stack;

    SpecPtr visit_delim_join(duckdb::LogicalComparisonJoin &op) {
      switch (op.join_type) {
      case duckdb::JoinType::SINGLE:
      case duckdb::JoinType::MARK:
      case duckdb::JoinType::INNER:
      case duckdb::JoinType::LEFT:
        break;
      default:
        return unsupported("DELIM join type " +
                           duckdb::EnumUtil::ToString(op.join_type));
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::DELIM_JOIN;
      spec->join_type = op.join_type == duckdb::JoinType::SINGLE
                            ? duckdb::JoinType::LEFT
                            : op.join_type;
      for (const auto &cond : op.conditions) {
        // DuckDB 2.0: a non-comparison JoinCondition carries a single-sided
        // ON predicate (the old LogicalComparisonJoin::predicate); the
        // left/right/comparison accessors throw on one.
        if (!cond.IsComparison()) {
          return unsupported("DELIM join with a single-sided ON predicate");
        }
        PlanOpSpec::JoinCond jc{cond.LeftReference().get(),
                                cond.RightReference().get(),
                                cond.GetComparisonType()};
        switch (cond.GetComparisonType()) {
        case duckdb::ExpressionType::COMPARE_NOT_DISTINCT_FROM:
          spec->null_safe_keys = true;
          spec->equi_conds.push_back(jc);
          break;
        case duckdb::ExpressionType::COMPARE_EQUAL:
          spec->equi_conds.push_back(jc);
          break;
        default:
          return unsupported("DELIM join comparison " +
                             duckdb::EnumUtil::ToString(cond.GetComparisonType()));
        }
      }

      auto left = visit(*op.children[0]);
      if (!left) {
        return nullptr;
      }
      std::vector<ColumnInfo> left_columns = columns;

      // Correlated (duplicate-eliminated) columns, evaluated over the left
      // output; DELIM_GET leaves in the right subplan read their DISTINCT
      std::vector<ColumnInfo> delim_cols;
      for (duckdb::idx_t i = 0; i < op.duplicate_eliminated_columns.size();
           i++) {
        const auto &e = op.duplicate_eliminated_columns[i];
        spec->exprs.push_back(e.get());
        delim_cols.push_back(
            {"delim_" + std::to_string(i), e->GetReturnType()});
      }
      if (spec->exprs.empty()) {
        return unsupported("DELIM join without correlated columns");
      }

      delim_columns_stack.push_back(delim_cols);
      auto right = visit(*op.children[1]);
      delim_columns_stack.pop_back();
      if (!right) {
        return nullptr;
      }

      spec->left_types = op.children[0]->types;
      spec->right_types = op.children[1]->types;
      spec->children.push_back(std::move(left));
      spec->children.push_back(std::move(right));

      if (op.join_type == duckdb::JoinType::MARK) {
        columns = std::move(left_columns);
        columns.push_back({"mark", duckdb::LogicalType::BOOLEAN});
      } else {
        std::vector<ColumnInfo> combined = std::move(left_columns);
        combined.insert(combined.end(), columns.begin(), columns.end());
        columns = std::move(combined);
      }
      return spec;
    }

    SpecPtr visit_delim_get(duckdb::LogicalDelimGet &op) {
      if (delim_columns_stack.empty()) {
        return unsupported("DELIM_GET outside a DELIM join");
      }
      (void)op;
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::DELIM_REF;
      columns = delim_columns_stack.back();
      return spec;
    }

    SpecPtr visit_join(duckdb::LogicalComparisonJoin &op) {
      switch (op.join_type) {
      case duckdb::JoinType::INNER:
      case duckdb::JoinType::LEFT:
      case duckdb::JoinType::RIGHT:
      case duckdb::JoinType::OUTER:
      case duckdb::JoinType::MARK:
        break;
      // JoinType::SINGLE is deliberately NOT here. DuckDB 2.0 rewrites a
      // correlated scalar subquery into materialized delim CTEs plus a
      // PLAIN comparison join of type SINGLE (1.5.4 produced a DELIM join,
      // which visit_delim_join maps SINGLE => LEFT). Adding the same
      // mapping here compiles and is semantically right, but does not make
      // the shape work: 2.0's rewrite also populates the join's
      // left/right_projection_map, which this path declines two checks
      // below. Accepting SINGLE alone would be untested code, so the shape
      // stays a decline until projection maps are supported. Pinned by
      // "planner E2: correlated scalar subquery declines on 2.0".
      default:
        return unsupported("join type " +
                           duckdb::EnumUtil::ToString(op.join_type));
      }
      if (!op.duplicate_eliminated_columns.empty()) {
        return unsupported("duplicate-eliminated (DELIM) join");
      }
      // DuckDB 2.0: LogicalComparisonJoin::predicate is gone — a single-sided
      // ON predicate is now carried as a non-comparison JoinCondition.
      for (const auto &cond : op.conditions) {
        if (!cond.IsComparison()) {
          return unsupported("join with single-sided ON predicate");
        }
      }
      if (!op.left_projection_map.empty() ||
          !op.right_projection_map.empty()) {
        return unsupported("join with projection maps");
      }

      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::JOIN;
      spec->join_type = op.join_type;
      for (const auto &cond : op.conditions) {
        PlanOpSpec::JoinCond jc{cond.LeftReference().get(),
                                cond.RightReference().get(),
                                cond.GetComparisonType()};
        switch (cond.GetComparisonType()) {
        case duckdb::ExpressionType::COMPARE_NOT_DISTINCT_FROM:
          // Null-safe equality (NULL matches NULL) — emitted by subquery
          // decorrelation; DuckDBRow key equality is already null-safe
          spec->null_safe_keys = true;
          spec->equi_conds.push_back(jc);
          break;
        case duckdb::ExpressionType::COMPARE_EQUAL:
          spec->equi_conds.push_back(jc);
          break;
        case duckdb::ExpressionType::COMPARE_GREATERTHAN:
        case duckdb::ExpressionType::COMPARE_LESSTHAN:
        case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
        case duckdb::ExpressionType::COMPARE_NOTEQUAL:
          spec->residual_conds.push_back(jc);
          break;
        default:
          return unsupported(
              "join comparison " +
              duckdb::EnumUtil::ToString(cond.GetComparisonType()));
        }
      }
      if (spec->null_safe_keys) {
        // null_safe applies to ALL equi keys of the node; mixing = and
        // IS NOT DISTINCT FROM in one join would null-match the = key too
        for (const auto &cond : op.conditions) {
          if (cond.GetComparisonType() == duckdb::ExpressionType::COMPARE_EQUAL) {
            return unsupported(
                "mixed null-safe and plain equality join keys");
          }
        }
      }

      auto left = visit(*op.children[0]);
      if (!left) {
        return nullptr;
      }
      std::vector<ColumnInfo> left_columns = columns;
      auto right = visit(*op.children[1]);
      if (!right) {
        return nullptr;
      }

      spec->left_types = op.children[0]->types;
      spec->right_types = op.children[1]->types;
      spec->children.push_back(std::move(left));
      spec->children.push_back(std::move(right));

      if (op.join_type == duckdb::JoinType::MARK) {
        // MARK output: left columns plus one three-valued match column
        columns = std::move(left_columns);
        columns.push_back({"mark", duckdb::LogicalType::BOOLEAN});
        return spec;
      }
      // Output: left columns then right columns
      std::vector<ColumnInfo> combined = std::move(left_columns);
      combined.insert(combined.end(), columns.begin(), columns.end());
      columns = std::move(combined);
      return spec;
    }

    SpecPtr visit_cross_product(duckdb::LogicalOperator &op) {
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::JOIN; // no conditions = cross product

      auto left = visit(*op.children[0]);
      if (!left) {
        return nullptr;
      }
      std::vector<ColumnInfo> left_columns = columns;
      auto right = visit(*op.children[1]);
      if (!right) {
        return nullptr;
      }

      spec->left_types = op.children[0]->types;
      spec->right_types = op.children[1]->types;
      spec->children.push_back(std::move(left));
      spec->children.push_back(std::move(right));

      std::vector<ColumnInfo> combined = std::move(left_columns);
      combined.insert(combined.end(), columns.begin(), columns.end());
      columns = std::move(combined);
      return spec;
    }

    SpecPtr visit_distinct(duckdb::LogicalDistinct &op) {
      if (op.distinct_type == duckdb::DistinctType::DISTINCT_ON) {
        return visit_distinct_on(op);
      }
      if (op.distinct_type != duckdb::DistinctType::DISTINCT) {
        return unsupported("DISTINCT variant");
      }
      // Plain DISTINCT deduplicates whole rows; targets must be the full
      // identity column list, otherwise semantics differ
      const auto &child_types = op.children[0]->types;
      if (op.distinct_targets.size() != child_types.size()) {
        return unsupported("DISTINCT over a subset of columns");
      }
      for (duckdb::idx_t i = 0; i < op.distinct_targets.size(); i++) {
        auto &target = op.distinct_targets[i];
        if (target->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_REF) {
          return unsupported("DISTINCT over computed targets");
        }
        if (target->Cast<duckdb::BoundReferenceExpression>().Index() != i) {
          return unsupported("DISTINCT with reordered targets");
        }
      }
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::DISTINCT;
      spec->children.push_back(std::move(child));
      // Row shape unchanged; columns stay as-is
      return spec;
    }

    SpecPtr visit_distinct_on(duckdb::LogicalDistinct &op) {
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::DISTINCT_ON;
      for (const auto &target : op.distinct_targets) {
        if (target->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_REF) {
          return unsupported("DISTINCT ON computed expression "
                             "(use a plain column)");
        }
        spec->column_idxs.push_back(
            target->Cast<duckdb::BoundReferenceExpression>().Index());
      }
      // Winner-pick order (which row survives per key) rides on the DISTINCT
      // node itself; the ORDER_BY operator above is presentation only
      if (op.order_by) {
        for (const auto &o : op.order_by->orders) {
          if (o.expression->GetExpressionClass() !=
              duckdb::ExpressionClass::BOUND_REF) {
            return unsupported("DISTINCT ON ORDER BY expression "
                               "(use a plain column)");
          }
          auto &ref = o.expression->Cast<duckdb::BoundReferenceExpression>();
          NativeSortView::SortColumn sc;
          sc.column_idx = static_cast<size_t>(ref.Index());
          sc.ascending = o.type != duckdb::OrderType::DESCENDING;
          sc.nulls_first =
              o.null_order == duckdb::OrderByNullType::NULLS_FIRST;
          spec->sort_columns.push_back(sc);
        }
      }
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }
      spec->children.push_back(std::move(child));
      // Output keeps the full child row layout; columns stay as-is
      return spec;
    }

    SpecPtr visit_set_operation(duckdb::LogicalSetOperation &op) {
      bool is_union =
          op.type == duckdb::LogicalOperatorType::LOGICAL_UNION;
      if (!is_union && op.children.size() != 2) {
        return unsupported("n-ary INTERSECT/EXCEPT");
      }

      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::SET_OP;
      if (is_union) {
        spec->set_op = op.setop_all ? PlanOpSpec::SetOp::UNION_ALL
                                    : PlanOpSpec::SetOp::UNION;
      } else if (op.type ==
                 duckdb::LogicalOperatorType::LOGICAL_INTERSECT) {
        spec->set_op = op.setop_all ? PlanOpSpec::SetOp::INTERSECT_ALL
                                    : PlanOpSpec::SetOp::INTERSECT;
      } else {
        spec->set_op = op.setop_all ? PlanOpSpec::SetOp::EXCEPT_ALL
                                    : PlanOpSpec::SetOp::EXCEPT;
      }

      std::vector<ColumnInfo> first_columns;
      for (size_t i = 0; i < op.children.size(); i++) {
        auto child = visit(*op.children[i]);
        if (!child) {
          return nullptr;
        }
        if (i == 0) {
          first_columns = columns;
        }
        spec->children.push_back(std::move(child));
      }
      // Set operation output takes the first child's column names
      columns = std::move(first_columns);
      return spec;
    }

    // Extract a column index from a bound expression; -1 if not a plain ref
    static int column_ref(const duckdb::Expression &expr) {
      if (expr.GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) {
        return -1;
      }
      return static_cast<int>(
          expr.Cast<duckdb::BoundReferenceExpression>().Index());
    }

    // Extract a constant int64 from a bare BOUND_CONSTANT; false otherwise.
    // Does NOT unwrap casts -- see constant_int() below for why that
    // matters.
    static bool bare_constant_int(const duckdb::Expression &expr,
                                  int64_t &out) {
      if (expr.GetExpressionClass() !=
          duckdb::ExpressionClass::BOUND_CONSTANT) {
        return false;
      }
      const auto &val = expr.Cast<duckdb::BoundConstantExpression>().GetValue();
      if (val.IsNull() || !val.type().IsNumeric()) {
        return false;
      }
      out = val.GetValue<int64_t>();
      return true;
    }

    // Extract a constant int64; false if not a non-NULL constant. The
    // binder wraps frame/offset literals in a cast (e.g. `11 PRECEDING`
    // arrives as CAST(11 AS BIGINT)), so unwrap any chain of cast nodes
    // before checking for BOUND_CONSTANT underneath. In DuckDB 2.0 a bound
    // cast is a BoundFunctionExpression (the `__cast` scalar function) —
    // see the inline comment in the loop below.
    //
    // Used for window frame bounds (start_expr/end_expr), LAG/LEAD offsets,
    // and -- as of the lazy-restore-ntile fix -- NTILE's bucket count:
    // NativeWindowView's NTILE bucket-boundary math (both render call
    // sites in include/dbsp_window_view.hpp) was rewritten to match stock
    // DuckDB's WindowNtileExecutor first, so widening the gate here no
    // longer ships a silently-wrong result. NTH_VALUE followed the same
    // path (Aug 2026): its render is frame-relative now (nth row of the
    // FRAME, both call sites in dbsp_window_view.hpp), so its N uses
    // constant_int like every other frame literal.
    static bool constant_int(const duckdb::Expression &expr, int64_t &out) {
      const duckdb::Expression *cur = &expr;
      // DuckDB 2.0: a bound cast is a BoundFunctionExpression (the __cast
      // scalar function); ExpressionClass::BOUND_CAST is gone and
      // BoundCastExpression is now a set of static helpers over it.
      while (duckdb::BoundCastExpression::IsCast(*cur)) {
        cur = &duckdb::BoundCastExpression::Child(
            cur->Cast<duckdb::BoundFunctionExpression>());
      }
      return bare_constant_int(*cur, out);
    }

    // True for a constant NULL (through any cast chain). DuckDB 2.0's
    // lead/lag signature declares defaults for its optional arguments
    // (offset = 1, default = NULL), so the binder pads every LEAD/LAG to
    // three children — a padded NULL default means "no default", exactly as
    // an absent BoundWindowExpression::default_expr did before 2.0.
    static bool constant_null(const duckdb::Expression &expr) {
      const duckdb::Expression *cur = &expr;
      while (duckdb::BoundCastExpression::IsCast(*cur)) {
        cur = &duckdb::BoundCastExpression::Child(
            cur->Cast<duckdb::BoundFunctionExpression>());
      }
      if (cur->GetExpressionClass() !=
          duckdb::ExpressionClass::BOUND_CONSTANT) {
        return false;
      }
      return cur->Cast<duckdb::BoundConstantExpression>().GetValue().IsNull();
    }

    SpecPtr visit_window(duckdb::LogicalWindow &op) {
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::WINDOW;
      const std::vector<ColumnInfo> original_cols = columns;
      spec->window_source_cols = columns;

      // M1: PARTITION BY / ORDER BY / argument expressions that are not
      // plain column refs get projected into helper columns below the
      // window (a MAP_EXPR computing [child cols..., expr...]) and
      // stripped again above it — the user-visible layout is unchanged.
      const size_t ncols = op.children[0]->types.size();
      std::vector<const duckdb::Expression *> helper_exprs;
      auto resolve_col = [&](const duckdb::Expression &e) -> int {
        int idx = column_ref(e);
        if (idx >= 0) {
          return idx;
        }
        const std::string repr = e.ToString();
        for (size_t k = 0; k < helper_exprs.size(); k++) {
          if (helper_exprs[k]->ToString() == repr) {
            return static_cast<int>(ncols + k);
          }
        }
        helper_exprs.push_back(&e);
        return static_cast<int>(ncols + helper_exprs.size() - 1);
      };

      for (const auto &expr : op.expressions) {
        if (expr->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_WINDOW) {
          return unsupported("non-window expression in WINDOW");
        }
        auto &w = expr->Cast<duckdb::BoundWindowExpression>();
        if (w.Filter() || w.Distinct() || w.IgnoreNulls() ||
            !w.ArgOrders().empty() ||
            w.WindowExclude() != duckdb::WindowExcludeMode::NO_OTHER) {
          return unsupported("window FILTER/DISTINCT/IGNORE NULLS/EXCLUDE");
        }

        NativeWindowView::WindowDef def;
        def.alias = expr->GetName().GetIdentifierName();
        def.start = w.WindowStart();
        def.end = w.WindowEnd();

        for (const auto &p : w.Partitions()) {
          def.partition_indices.push_back(
              static_cast<size_t>(resolve_col(*p)));
        }
        for (const auto &o : w.OrderBy()) {
          NativeSortView::SortColumn sc;
          sc.column_idx = static_cast<size_t>(resolve_col(*o.expression));
          sc.ascending = o.type != duckdb::OrderType::DESCENDING;
          sc.nulls_first = o.null_order == duckdb::OrderByNullType::NULLS_FIRST;
          def.sort_columns.push_back(sc);
        }

        int64_t n = 0;
        switch (w.GetExpressionType()) {
        case duckdb::ExpressionType::WINDOW_ROW_NUMBER:
          def.function = "ROW_NUMBER";
          break;
        case duckdb::ExpressionType::WINDOW_RANK:
          def.function = "RANK";
          break;
        case duckdb::ExpressionType::WINDOW_RANK_DENSE:
          def.function = "DENSE_RANK";
          break;
        case duckdb::ExpressionType::WINDOW_NTILE:
          def.function = "NTILE";
          if (w.GetChildren().empty() || !constant_int(*w.GetChildren()[0], n)) {
            return unsupported("NTILE with non-constant bucket count");
          }
          def.offset = static_cast<int>(n);
          break;
        case duckdb::ExpressionType::WINDOW_LAG:
        case duckdb::ExpressionType::WINDOW_LEAD: {
          def.function = w.GetExpressionType() ==
                                 duckdb::ExpressionType::WINDOW_LAG
                             ? "LAG"
                             : "LEAD";
          if (w.GetChildren().empty()) {
            return unsupported(def.function + " without argument");
          }
          def.arg_column_idx = resolve_col(*w.GetChildren()[0]);
          def.offset = 1;
          // DuckDB 2.0: LEAD/LAG carry their offset and default as children
          // [1] and [2] (BoundWindowExpression::offset_expr/default_expr are
          // gone) — see WindowLeadLagStreamingState::ComputeOffset/Default.
          if (w.GetChildren().size() > 1 && w.GetChildren()[1]) {
            if (!constant_int(*w.GetChildren()[1], n)) {
              return unsupported(def.function + " with non-constant offset");
            }
            def.offset = static_cast<int>(n);
          }
          if (w.GetChildren().size() > 2 && w.GetChildren()[2] &&
              !constant_null(*w.GetChildren()[2])) {
            return unsupported(def.function + " with a default value");
          }
          break;
        }
        case duckdb::ExpressionType::WINDOW_FIRST_VALUE:
        case duckdb::ExpressionType::WINDOW_LAST_VALUE:
        case duckdb::ExpressionType::WINDOW_NTH_VALUE: {
          auto t = w.GetExpressionType();
          def.function =
              t == duckdb::ExpressionType::WINDOW_FIRST_VALUE
                  ? "FIRST_VALUE"
                  : t == duckdb::ExpressionType::WINDOW_LAST_VALUE
                        ? "LAST_VALUE"
                        : "NTH_VALUE";
          if (w.GetChildren().empty()) {
            return unsupported(def.function + " without argument");
          }
          def.arg_column_idx = resolve_col(*w.GetChildren()[0]);
          if (t == duckdb::ExpressionType::WINDOW_NTH_VALUE) {
            // constant_int (cast-unwrapping), not bare_constant_int: the
            // render is frame-relative now (see dbsp_window_view.hpp), so
            // the deliberate narrow gate documented above is lifted.
            if (w.GetChildren().size() < 2 ||
                !constant_int(*w.GetChildren()[1], n)) {
              return unsupported("NTH_VALUE with non-constant N");
            }
            def.offset = static_cast<int>(n);
          }
          break;
        }
        case duckdb::ExpressionType::WINDOW_AGGREGATE: {
          std::string fn =
              duckdb::StringUtil::Upper(w.AggregateFunction()
                                        ? w.AggregateFunction()->GetName().GetIdentifierName()
                                        : std::string());
          if (fn == "COUNT_STAR") {
            fn = "COUNT";
          }
          if (fn != "SUM" && fn != "COUNT" && fn != "AVG" && fn != "MIN" &&
              fn != "MAX") {
            return unsupported("window aggregate " + fn);
          }
          def.function = fn;
          if (!w.GetChildren().empty()) {
            def.arg_column_idx = resolve_col(*w.GetChildren()[0]);
          }
          break;
        }
        default:
          return unsupported(
              "window function " +
              duckdb::EnumUtil::ToString(w.GetExpressionType()));
        }

        // Frame offsets must be constants when boundaries use expressions
        if (w.WindowStart() == duckdb::WindowBoundary::EXPR_PRECEDING_ROWS) {
          if (!w.StartExpr() || !constant_int(*w.StartExpr(), n)) {
            return unsupported("non-constant frame start");
          }
          def.start_offset = static_cast<int>(n);
        }
        if (w.WindowEnd() == duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS) {
          if (!w.EndExpr() || !constant_int(*w.EndExpr(), n)) {
            return unsupported("non-constant frame end");
          }
          def.end_offset = static_cast<int>(n);
        }

        // NativeWindowView partitions all windows by the FIRST window's
        // partition/order; additional windows must match it
        if (!spec->window_defs.empty()) {
          const auto &first = spec->window_defs[0];
          bool same = first.partition_indices == def.partition_indices &&
                      first.sort_columns.size() == def.sort_columns.size();
          for (size_t i = 0; same && i < first.sort_columns.size(); i++) {
            same = first.sort_columns[i].column_idx ==
                       def.sort_columns[i].column_idx &&
                   first.sort_columns[i].ascending ==
                       def.sort_columns[i].ascending &&
                   first.sort_columns[i].nulls_first ==
                       def.sort_columns[i].nulls_first;
          }
          if (!same) {
            return unsupported("multiple windows with different "
                               "PARTITION BY / ORDER BY clauses");
          }
        }
        spec->window_defs.push_back(std::move(def));
      }

      // Output: child columns then one column per window expression
      const size_t num_windows = op.expressions.size();
      std::vector<ColumnInfo> window_cols;
      for (duckdb::idx_t i = 0; i < num_windows; i++) {
        window_cols.push_back(
            {op.expressions[i]->GetName().GetIdentifierName(), op.types[ncols + i]});
      }

      if (helper_exprs.empty()) {
        columns = original_cols;
        columns.insert(columns.end(), window_cols.begin(),
                       window_cols.end());
        spec->window_result_cols = columns;
        spec->children.push_back(std::move(child));
        return spec;
      }

      // Sandwich: MAP (add helper cols) → WINDOW → MAP (strip them)
      const size_t nhelp = helper_exprs.size();
      auto pre_map = std::make_unique<PlanOpSpec>();
      pre_map->kind = PlanOpSpec::Kind::MAP_EXPR;
      pre_map->input_types = op.children[0]->types;
      std::vector<ColumnInfo> widened_cols = original_cols;
      duckdb::vector<duckdb::LogicalType> widened_types =
          op.children[0]->types;
      for (size_t c = 0; c < ncols; c++) {
        auto e = duckdb::make_uniq<duckdb::BoundReferenceExpression>(
            op.children[0]->types[c], c);
        pre_map->exprs.push_back(e.get());
        owned_exprs.push_back(std::move(e));
      }
      for (size_t k = 0; k < nhelp; k++) {
        pre_map->exprs.push_back(helper_exprs[k]);
        widened_cols.push_back({"__w_expr_" + std::to_string(k),
                                helper_exprs[k]->GetReturnType()});
        widened_types.push_back(helper_exprs[k]->GetReturnType());
      }
      pre_map->children.push_back(std::move(child));

      spec->window_source_cols = widened_cols;
      std::vector<ColumnInfo> window_out_cols = widened_cols;
      window_out_cols.insert(window_out_cols.end(), window_cols.begin(),
                             window_cols.end());
      spec->window_result_cols = window_out_cols;
      spec->children.push_back(std::move(pre_map));

      auto post_map = std::make_unique<PlanOpSpec>();
      post_map->kind = PlanOpSpec::Kind::MAP_EXPR;
      post_map->input_types = widened_types;
      for (duckdb::idx_t i = 0; i < num_windows; i++) {
        post_map->input_types.push_back(op.types[ncols + i]);
      }
      for (size_t c = 0; c < ncols; c++) {
        auto e = duckdb::make_uniq<duckdb::BoundReferenceExpression>(
            op.children[0]->types[c], c);
        post_map->exprs.push_back(e.get());
        owned_exprs.push_back(std::move(e));
      }
      for (duckdb::idx_t i = 0; i < num_windows; i++) {
        auto e = duckdb::make_uniq<duckdb::BoundReferenceExpression>(
            op.types[ncols + i], ncols + nhelp + i);
        post_map->exprs.push_back(e.get());
        owned_exprs.push_back(std::move(e));
      }
      post_map->children.push_back(std::move(spec));

      columns = original_cols;
      columns.insert(columns.end(), window_cols.begin(), window_cols.end());
      return post_map;
    }

    SpecPtr visit_cte(duckdb::LogicalMaterializedCTE &op) {
      auto def = visit(*op.children[0]);
      if (!def) {
        return nullptr;
      }
      cte_columns[op.table_index.index] = columns;
      auto main = visit(*op.children[1]);
      if (!main) {
        return nullptr;
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::CTE;
      spec->cte_index = op.table_index.index;
      spec->children.push_back(std::move(def));
      spec->children.push_back(std::move(main));
      // columns already reflect the main query's output
      return spec;
    }

    // WITH RECURSIVE: the step's self-reference becomes a SOURCE with a
    // sentinel table name routed inside the PlanRecursiveNode's inner view
    std::set<duckdb::idx_t> recursive_cte_indexes;

    static std::string rec_cte_sentinel(duckdb::idx_t index) {
      return "__rec_cte_" + std::to_string(index) + "__";
    }

    SpecPtr visit_recursive_cte(duckdb::LogicalRecursiveCTE &op) {
      if (!op.key_targets.empty()) {
        return unsupported("recursive CTE USING KEY");
      }
      recursive_cte_indexes.insert(op.table_index.index);
      auto anchor = visit(*op.children[0]);
      if (!anchor) {
        return nullptr;
      }
      // Output layout = anchor layout (ResolveTypes: types = children[0])
      auto anchor_cols = columns;
      cte_columns[op.table_index.index] = anchor_cols; // step's CTE_SCAN resolves
      auto step = visit(*op.children[1]);
      if (!step) {
        return nullptr;
      }
      // Row-collapsing operators inside a recursive STEP (AGGREGATE /
      // DISTINCT / DISTINCT_ON / WINDOW / SORT_LIMIT / non-UNION-ALL set
      // ops) do not obey the fixpoint's frontier semantics on ANY current
      // execution path — the step iterates on frontiers and accumulated
      // state, not SQL's per-iteration working table, so results silently
      // diverge from stock recursion. These shapes used to be ACCEPTED
      // and wrong (hosts had to guard themselves, e.g. NuEPM's
      // assert_step_linear); reject loudly instead.
      PlannedCircuitView::StepLinearity step_shape;
      PlannedCircuitView::scan_step_linearity(
          *step, rec_cte_sentinel(op.table_index.index), step_shape);
      if (step_shape.has_weight_nonlinear_op) {
        return unsupported(
            "recursive step contains a row-collapsing operator "
            "(DISTINCT/GROUP BY/LIMIT/window/set-op): results would "
            "silently diverge from SQL recursion semantics");
      }
      columns = std::move(anchor_cols);
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::REC_CTE;
      spec->set_op = op.union_all ? PlanOpSpec::SetOp::UNION_ALL
                                  : PlanOpSpec::SetOp::UNION;
      spec->cte_index = op.table_index.index;
      spec->children.push_back(std::move(anchor));
      spec->children.push_back(std::move(step));
      return spec;
    }

    SpecPtr visit_cte_ref(duckdb::LogicalCTERef &op) {
      if (recursive_cte_indexes.count(op.cte_index.index)) {
        auto rec_it = cte_columns.find(op.cte_index.index);
        if (rec_it == cte_columns.end()) {
          return unsupported("self-reference outside its recursive CTE");
        }
        auto spec = std::make_unique<PlanOpSpec>();
        spec->kind = PlanOpSpec::Kind::SOURCE;
        spec->table = rec_cte_sentinel(op.cte_index.index);
        columns.clear();
        for (duckdb::idx_t i = 0; i < op.chunk_types.size(); i++) {
          std::string name = i < op.bound_columns.size()
                                 ? op.bound_columns[i].GetIdentifierName()
                                 : rec_it->second[i].name;
          columns.push_back({name, op.chunk_types[i]});
        }
        return spec;
      }
      auto it = cte_columns.find(op.cte_index.index);
      if (it == cte_columns.end()) {
        return unsupported("reference to an untranslated CTE");
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::CTE_REF;
      spec->cte_index = op.cte_index.index;
      columns.clear();
      for (duckdb::idx_t i = 0; i < op.chunk_types.size(); i++) {
        std::string name = i < op.bound_columns.size()
                               ? op.bound_columns[i].GetIdentifierName()
                               : it->second[i].name;
        columns.push_back({name, op.chunk_types[i]});
      }
      return spec;
    }

    std::unordered_map<duckdb::idx_t, std::vector<ColumnInfo>> cte_columns;

    SpecPtr visit_get(duckdb::LogicalGet &op) {
      auto table_entry = op.GetTable();
      if (!table_entry) {
        return unsupported("non-table scan (" + op.function.name + ")");
      }
      if (op.table_filters.HasFilters()) {
        return unsupported("GET with pushed-down table filters");
      }
      if (!op.projection_ids.empty()) {
        return unsupported("GET with projection ids");
      }

      auto source = std::make_unique<PlanOpSpec>();
      source->kind = PlanOpSpec::Kind::SOURCE;
      // Canonical catalog.schema.table key (D2): derived from the bound
      // entry, never from SQL text, so attached-catalog tables and bare
      // names key identically everywhere in the CDC layer. Temp-catalog
      // entries keep their bare name: views-on-views bind through TEMP
      // shadow tables whose names must match the referenced view's key.
      if (table_entry->ParentCatalog().GetName() == TEMP_CATALOG) {
        source->table = table_entry->name.GetIdentifierName();
      } else {
        source->table = canonical_table_key(*table_entry);
      }
      // Full-table layout (CDC row shape) — arrangement key remapping
      // needs it (O4 cross-projection sharing)
      source->input_types = op.returned_types;

      // CDC rows carry ALL table columns in declared order; GET's output is
      // column_ids order. Emit an index-selection map unless it's identity.
      // Virtual columns (rowid) become NULL: CDC deltas have no stable row
      // ids. Plans only request them when nothing reads the value (e.g. the
      // binder projects rowid as the cheapest column for COUNT(*)).
      const auto &column_ids = op.GetColumnIds();
      std::vector<duckdb::idx_t> idxs;
      bool identity = column_ids.size() == op.returned_types.size();
      columns.clear();
      for (duckdb::idx_t i = 0; i < column_ids.size(); i++) {
        duckdb::idx_t col = column_ids[i].GetPrimaryIndex();
        if (col >= op.returned_types.size()) {
          // Out-of-range index: the MAP_COLS lambda emits NULL for it
          identity = false;
          idxs.push_back(col);
          columns.push_back({"rowid", op.types[i]});
          continue;
        }
        if (col != i) {
          identity = false;
        }
        idxs.push_back(col);
        columns.push_back({op.names[col].GetIdentifierName(), op.returned_types[col]});
      }
      if (identity) {
        return source;
      }
      auto select = std::make_unique<PlanOpSpec>();
      select->kind = PlanOpSpec::Kind::MAP_COLS;
      select->column_idxs = std::move(idxs);
      // CDC full-row layout (declared column order): the fuse_map_cols IR
      // pass needs the source arity and the fused node needs this as its
      // chunk layout
      select->input_types.assign(op.returned_types.begin(),
                                 op.returned_types.end());
      select->children.push_back(std::move(source));
      return select;
    }
  };
};

} // namespace dbsp_native
