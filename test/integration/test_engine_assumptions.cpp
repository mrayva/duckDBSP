// Engine-assumption canaries. The auto-CDC hooks stand on empirically probed
// DuckDB behaviour that no API contract guarantees. Each TEST_CASE here pins
// ONE assumption by name; on an engine upgrade these fail FIRST, with a
// readable message, instead of surfacing as mystery view corruption in the
// differential suites.
//
// The plan-shape canaries that used to live here (DML row references, the
// UPDATE rowid-last convention, projection-pushdown bindings,
// update_is_del_and_insert, INSERT child width) went with the optimizer plan
// tee they existed for: nothing reads or rewrites a DML plan any more, so an
// engine change to any of those shapes can no longer corrupt a delta.

#include "../test_helpers.hpp"
#include "catch.hpp"

using namespace dbsp_test;
using namespace duckdb;

TEST_CASE("canary: autocommit hook ordering", "[engine_assumptions]") {
  // The fold split (autocommit folds at QueryBegin, explicit txn at
  // QueryEnd, QueryEnd skips autocommit) and the choice of
  // TransactionCommit as the apply point both depend on: an autocommit
  // statement having an ACTIVE transaction with IsAutoCommit() true at
  // BOTH QueryBegin and QueryEnd, and its TransactionCommit hook firing
  // mid-statement (before QueryEnd).
  struct ProbeState : public ClientContextState {
    bool begin_active = false, begin_auto = false;
    bool end_active = false, end_auto = false;
    int commit_before_end = -1; // 1 = commit hook saw no QueryEnd yet
    bool saw_end = false;
    void QueryBegin(ClientContext &context) override {
      begin_active = context.transaction.HasActiveTransaction();
      begin_auto = context.transaction.IsAutoCommit();
    }
    void QueryEnd(ClientContext &context,
                  optional_ptr<ErrorData> error) override {
      saw_end = true;
      end_active = context.transaction.HasActiveTransaction();
      end_auto = context.transaction.IsAutoCommit();
    }
    void TransactionCommit(MetaTransaction &, ClientContext &) override {
      commit_before_end = saw_end ? 0 : 1;
    }
  };
  DuckDBTestHarness db;
  db.exec("CREATE TABLE ca (id INTEGER, val INTEGER, name VARCHAR)");
  db.exec("INSERT INTO ca VALUES (1, 10, 'x'), (2, 20, 'y')");
  auto probe = db.conn().context->registered_state->GetOrCreate<ProbeState>(
      "dbsp_canary_probe");
  db.exec("INSERT INTO ca VALUES (500, 5, 'p')");
  // QueryBegin: transaction active, autocommit — this is why the fold and
  // the trigger sweep both run there
  REQUIRE(probe->begin_active);
  REQUIRE(probe->begin_auto);
  // TransactionCommit fires MID-statement, before QueryEnd — buffered
  // deltas apply there
  REQUIRE(probe->commit_before_end == 1);
  // QueryEnd: the transaction is GONE — nothing may fold or resolve
  // catalog entries here for autocommit statements
  REQUIRE_FALSE(probe->end_active);
}
