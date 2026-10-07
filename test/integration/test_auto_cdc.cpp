#include "../test_helpers.hpp"
#include "catch.hpp"
#include <random>

using namespace dbsp_test;
using namespace std;

TEST_CASE("Automatic CDC via Transaction Hooks", "[dbsp][auto_cdc]") {
  DuckDBTestHarness db;

  // Setup table
  db.createTable("items", "id INTEGER, name VARCHAR, price DECIMAL(10,2)", {});
  db.exec("SELECT * FROM dbsp_track('items')");

  // Create view (use SELECT * since projection returns all columns)
  db.exec("SELECT * FROM dbsp_create_view('expensive_items', "
          "'SELECT * FROM items WHERE price > 50')");

  SECTION("Manual sync updates views correctly") {
    // Insert data using standard SQL
    db.exec("INSERT INTO items VALUES (1, 'Widget', 10.00)");
    db.exec("INSERT INTO items VALUES (2, 'Gadget', 100.00)");

    // Sync required until automatic CDC (P4.1) is implemented
    db.exec("SELECT * FROM dbsp_sync('items')");

    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 1);
    // Columns: id, name, price
    REQUIRE(rows[0][1].ToString() == "Gadget");
  }

  SECTION("Sync after transaction commit") {
    db.exec("BEGIN TRANSACTION");
    db.exec("INSERT INTO items VALUES (3, 'Premium', 200.00)");
    db.exec("INSERT INTO items VALUES (4, 'Cheap', 20.00)");

    db.exec("COMMIT");

    // Sync required until automatic CDC (P4.1) is implemented
    db.exec("SELECT * FROM dbsp_sync('items')");

    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 1);
    // Columns: id, name, price
    REQUIRE(rows[0][1].ToString() == "Premium");
  }

  SECTION("No data after rollback and sync") {
    db.exec("BEGIN TRANSACTION");
    db.exec("INSERT INTO items VALUES (5, 'Mistake', 500.00)");

    db.exec("ROLLBACK");

    db.exec("SELECT * FROM dbsp_sync('items')");

    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 0);
  }

  SECTION("Auto-sync without manual dbsp_sync") {
    // Enable automatic CDC
    db.exec("SELECT * FROM dbsp_auto_sync(true)");

    // Insert data using standard SQL
    db.exec("INSERT INTO items VALUES (10, 'AutoWidget', 75.00)");
    db.exec("INSERT INTO items VALUES (11, 'AutoGadget', 150.00)");

    // NO manual sync - auto-sync should handle it!
    // db.exec("SELECT * FROM dbsp_sync('items')"); // REMOVED

    // Views should be updated automatically
    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 2);

    // Verify both items are present
    bool found_widget = false;
    bool found_gadget = false;
    for (const auto &row : rows) {
      std::string name = row[1].ToString();
      if (name == "AutoWidget") found_widget = true;
      if (name == "AutoGadget") found_gadget = true;
    }
    REQUIRE(found_widget);
    REQUIRE(found_gadget);
  }

  SECTION("Auto-sync with explicit transaction") {
    // Enable automatic CDC
    db.exec("SELECT * FROM dbsp_auto_sync(true)");

    // Use explicit transaction
    db.exec("BEGIN TRANSACTION");
    db.exec("INSERT INTO items VALUES (20, 'TxnPremium', 300.00)");
    db.exec("INSERT INTO items VALUES (21, 'TxnCheap', 15.00)");
    db.exec("COMMIT");

    // NO manual sync - auto-sync should trigger on commit!
    // db.exec("SELECT * FROM dbsp_sync('items')"); // REMOVED

    // View should have exactly 1 expensive item
    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][1].ToString() == "TxnPremium");
  }

  SECTION("Auto-sync can be toggled on/off") {
    // Disable auto-sync
    db.exec("SELECT * FROM dbsp_auto_sync(false)");

    // Insert data
    db.exec("INSERT INTO items VALUES (30, 'NoAutoSync', 500.00)");

    // View should NOT be updated (auto-sync is off)
    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 0);

    // Manual sync should still work
    db.exec("SELECT * FROM dbsp_sync('items')");
    rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 1);

    // Re-enable auto-sync
    db.exec("SELECT * FROM dbsp_auto_sync(true)");
    db.exec("INSERT INTO items VALUES (31, 'AutoAgain', 600.00)");

    // Should auto-update now
    rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 2);
  }
}

// ===== Exact trigger-fed deltas: the fast path =====

TEST_CASE("exact deltas: explicit-txn inserts skip the scan",
          "[integration][auto_cdc][delta]") {
  DuckDBTestHarness db;
  db.createTable("ct", "id INT, val INT", {"(1, 10)"});
  db.exec("SELECT * FROM dbsp_track('ct')");
  db.exec("SELECT * FROM dbsp_sync('ct')");
  db.exec("SELECT * FROM dbsp_create_view('v_cap', "
          "'SELECT id, val FROM ct WHERE val > 5')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  auto &manager = db.manager();
  const uint64_t before = manager.exact_delta_syncs();

  db.exec("BEGIN");
  db.exec("INSERT INTO ct VALUES (2, 20), (3, 3)");
  db.exec("INSERT INTO ct VALUES (4, 40)");
  db.exec("COMMIT");

  // Fast path must have served the commit (no scan-and-diff)...
  REQUIRE(manager.exact_delta_syncs() == before + 1);
  // ...and the view must be correct: vals 10, 20, 40 pass (3 filtered)
  db.assertViewRowCount("v_cap", 3);

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("exact deltas: txn mixing insert and same-table delete",
          "[integration][auto_cdc][delta]") {
  DuckDBTestHarness db;
  db.createTable("ct2", "id INT, val INT", {"(1, 10)", "(2, 20)"});
  db.exec("SELECT * FROM dbsp_track('ct2')");
  db.exec("SELECT * FROM dbsp_sync('ct2')");
  db.exec("SELECT * FROM dbsp_create_view('v_cap2', "
          "'SELECT id FROM ct2 WHERE val > 5')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  auto &manager = db.manager();
  const uint64_t before = manager.exact_delta_syncs();

  db.exec("BEGIN");
  db.exec("INSERT INTO ct2 VALUES (3, 30)");
  // the DELETE runs against a table this transaction already wrote; both
  // firings buffer into one delta — still a single O(delta) apply
  db.exec("DELETE FROM ct2 WHERE id = 1");
  db.exec("COMMIT");

  REQUIRE(manager.exact_delta_syncs() == before + 1);
  db.assertViewRowCount("v_cap2", 2);

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("exact deltas: rolled-back txn leaves views untouched",
          "[integration][auto_cdc][delta]") {
  DuckDBTestHarness db;
  db.createTable("ct3", "id INT, val INT", {"(1, 10)"});
  db.exec("SELECT * FROM dbsp_track('ct3')");
  db.exec("SELECT * FROM dbsp_sync('ct3')");
  db.exec("SELECT * FROM dbsp_create_view('v_cap3', "
          "'SELECT id FROM ct3 WHERE val > 5')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  db.exec("BEGIN");
  db.exec("INSERT INTO ct3 VALUES (2, 20)");
  db.exec("ROLLBACK");

  db.assertViewRowCount("v_cap3", 1); // buffered rows never happened

  // A subsequent committed txn works normally
  db.exec("BEGIN");
  db.exec("INSERT INTO ct3 VALUES (3, 30)");
  db.exec("COMMIT");
  db.assertViewRowCount("v_cap3", 2);

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("exact deltas: autocommit INSERT sources",
          "[integration][auto_cdc][delta]") {
  DuckDBTestHarness db;
  db.createTable("ct4", "id INT, val INT", {"(1, 10)"});
  db.exec("SELECT * FROM dbsp_track('ct4')");
  db.exec("SELECT * FROM dbsp_sync('ct4')");
  db.exec("SELECT * FROM dbsp_create_view('v_cap4', "
          "'SELECT id FROM ct4 WHERE val > 5')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");
  auto &manager = db.manager();

  // Plain VALUES: exact delta, no scan
  uint64_t caps = manager.exact_delta_syncs();
  uint64_t scans = manager.scan_syncs();
  db.exec("INSERT INTO ct4 VALUES (2, 20)");
  REQUIRE(manager.exact_delta_syncs() == caps + 1);
  REQUIRE(manager.scan_syncs() == scans);
  db.assertViewRowCount("v_cap4", 2);

  // INSERT ... SELECT over base tables: exact too — the trigger reports the
  // rows the statement appended, whatever produced them
  caps = manager.exact_delta_syncs();
  scans = manager.scan_syncs();
  db.exec("INSERT INTO ct4 SELECT id + 10, val + 10 FROM ct4 WHERE id = 2");
  REQUIRE(manager.exact_delta_syncs() == caps + 1);
  REQUIRE(manager.scan_syncs() == scans);
  db.assertViewRowCount("v_cap4", 3);

  // LIMIT source: WHICH rows are appended depends on scan order, so nothing
  // could re-derive them — the trigger reports the ones actually appended
  caps = manager.exact_delta_syncs();
  scans = manager.scan_syncs();
  db.exec("INSERT INTO ct4 SELECT id + 20, val FROM ct4 LIMIT 1");
  REQUIRE(manager.exact_delta_syncs() == caps + 1);
  REQUIRE(manager.scan_syncs() == scans);

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("exact deltas: randomized explicit-txn churn stays correct",
          "[integration][auto_cdc][delta]") {
  DuckDBTestHarness db;
  db.createTable("cr", "id INT, val INT", {"(0, 5)"});
  db.exec("SELECT * FROM dbsp_track('cr')");
  db.exec("SELECT * FROM dbsp_sync('cr')");
  db.exec("SELECT * FROM dbsp_create_view('v_cr', "
          "'SELECT id, val FROM cr WHERE val > 10')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  std::mt19937 rng(4242);
  for (int round = 0; round < 30; round++) {
    db.exec("BEGIN");
    int inserts = static_cast<int>(rng() % 4) + 1;
    for (int i = 0; i < inserts; i++) {
      db.exec("INSERT INTO cr VALUES (" + std::to_string(round * 10 + i) +
              ", " + std::to_string(rng() % 40) + ")");
    }
    if (rng() % 4 == 0) {
      // ~25% of transactions also delete, so inserts and deletes mix
      db.exec("DELETE FROM cr WHERE id = " +
              std::to_string(rng() % (round * 10 + 1)));
    }
    if (rng() % 5 == 0) {
      db.exec("ROLLBACK");
    } else {
      db.exec("COMMIT");
    }

    auto expected = db.query("SELECT * FROM (SELECT id, val FROM cr "
                             "WHERE val > 10) ORDER BY ALL");
    auto actual =
        db.query("SELECT * FROM dbsp_query('v_cr') ORDER BY ALL");
    REQUIRE_FALSE(expected->HasError());
    REQUIRE_FALSE(actual->HasError());
    REQUIRE(actual->RowCount() == expected->RowCount());
    for (size_t r = 0; r < expected->RowCount(); r++) {
      for (size_t c = 0; c < expected->ColumnCount(); c++) {
        REQUIRE(actual->GetValue(c, r).ToString() ==
                expected->GetValue(c, r).ToString());
      }
    }
  }
  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("H1: commits that changed nothing scan nothing",
          "[integration][auto_cdc][scoping]") {
  DuckDBTestHarness db;
  db.createTable("sa", "id INT", {"(1)"});
  db.createTable("sb", "id INT", {"(1)"});
  db.exec("SELECT * FROM dbsp_track('sa')");
  db.exec("SELECT * FROM dbsp_track('sb')");
  db.exec("SELECT * FROM dbsp_sync('sa')");
  db.exec("SELECT * FROM dbsp_sync('sb')");
  db.exec("SELECT * FROM dbsp_create_view('v_sa', 'SELECT id FROM sa')");
  db.exec("SELECT * FROM dbsp_create_view('v_sb', 'SELECT id FROM sb')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  auto &manager = db.manager();

  // Autocommit DELETE on sa: served by an exact delta, no scan of either
  // table — and sb, which nothing wrote, is untouched.
  uint64_t scans = manager.scan_syncs();
  uint64_t caps = manager.exact_delta_syncs();
  db.exec("DELETE FROM sa WHERE id = 1");
  REQUIRE(manager.scan_syncs() == scans);
  REQUIRE(manager.exact_delta_syncs() == caps + 1);
  db.assertViewRowCount("v_sa", 0);
  db.assertViewRowCount("v_sb", 1);

  // Explicit txn UPDATE on sb: same, one apply
  scans = manager.scan_syncs();
  caps = manager.exact_delta_syncs();
  db.exec("BEGIN");
  db.exec("UPDATE sb SET id = 2 WHERE id = 1");
  db.exec("COMMIT");
  REQUIRE(manager.scan_syncs() == scans);
  REQUIRE(manager.exact_delta_syncs() == caps + 1);
  db.assertViewRowCount("v_sb", 1);

  // Read-only explicit txn: zero scans AND zero applies. This is the H1
  // early return — a commit that saw only reads has nothing to reconcile,
  // and before H1 it scan-diffed every tracked table.
  scans = manager.scan_syncs();
  caps = manager.exact_delta_syncs();
  db.exec("BEGIN");
  db.exec("SELECT * FROM sa");
  db.exec("COMMIT");
  REQUIRE(manager.scan_syncs() == scans);
  REQUIRE(manager.exact_delta_syncs() == caps);

  // A write matching NO rows evaluates the ingest scalar zero times, so the
  // commit falls back to the scan — and this is what proves the fallback is
  // still SCOPED: exactly ONE table is scanned, not both. Before H1 this was
  // a scan-and-diff of every tracked table.
  scans = manager.scan_syncs();
  db.exec("UPDATE sa SET id = 99 WHERE id = -1");
  REQUIRE(manager.scan_syncs() == scans + 1);
  db.assertViewRowCount("v_sa", 0);
  db.assertViewRowCount("v_sb", 1);

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

// ===== Differential matrix over UPDATE/DELETE/INSERT shapes =====
// Every scenario checks the views against direct SQL over the base tables,
// and asserts via counters WHICH path served the commit (exact delta vs
// scan). The counter half matters: a scan produces the RIGHT view too, so
// the view check alone cannot fail when the fast path silently stops
// working.

namespace {

// Base tables wt/wd/we + aggregate, join, and chained views
struct DeltaFixture {
  DuckDBTestHarness db;

  DeltaFixture() {
    db.createTable("wt", "id INT, grp INT, val INT",
                   {"(1, 1, 10)", "(2, 1, 20)", "(3, 2, 30)", "(4, 2, NULL)"});
    db.createTable("wd", "grp INT, name VARCHAR", {"(1, 'a')", "(2, 'b')"});
    db.createTable("we", "id INT, tag VARCHAR", {"(1, 'x')", "(2, 'y')"});
    db.exec("SELECT * FROM dbsp_track('wt')");
    db.exec("SELECT * FROM dbsp_track('wd')");
    db.exec("SELECT * FROM dbsp_track('we')");
    db.exec("SELECT * FROM dbsp_sync('wt')");
    db.exec("SELECT * FROM dbsp_sync('wd')");
    db.exec("SELECT * FROM dbsp_sync('we')");
    db.exec("SELECT * FROM dbsp_create_view('wv_agg', 'SELECT grp, "
            "SUM(val) AS s, COUNT(*) AS n FROM wt GROUP BY grp')");
    db.exec("SELECT * FROM dbsp_create_view('wv_join', 'SELECT wt.id, "
            "wt.val, wd.name FROM wt JOIN wd ON wt.grp = wd.grp')");
    db.exec("SELECT * FROM dbsp_create_view('wv_chain', "
            "'SELECT grp, s FROM wv_agg WHERE n >= 2')");
    db.exec("SELECT * FROM dbsp_auto_sync(true)");
  }

  void finish() { db.exec("SELECT * FROM dbsp_auto_sync(false)"); }

  // Every view must equal the same query run directly over base tables
  void check_views() {
    check("wv_agg",
          "SELECT grp, SUM(val) AS s, COUNT(*) AS n FROM wt GROUP BY grp");
    check("wv_join", "SELECT wt.id, wt.val, wd.name FROM wt JOIN wd "
                     "ON wt.grp = wd.grp");
    check("wv_chain", "SELECT grp, SUM(val) AS s FROM wt GROUP BY grp "
                      "HAVING COUNT(*) >= 2");
  }

  void check(const std::string &view, const std::string &sql) {
    auto expected = db.query("SELECT * FROM (" + sql + ") ORDER BY ALL");
    auto actual =
        db.query("SELECT * FROM dbsp_query('" + view + "') ORDER BY ALL");
    REQUIRE_FALSE(expected->HasError());
    REQUIRE_FALSE(actual->HasError());
    INFO("view " << view);
    REQUIRE(actual->RowCount() == expected->RowCount());
    for (size_t r = 0; r < expected->RowCount(); r++) {
      for (size_t c = 0; c < expected->ColumnCount(); c++) {
        REQUIRE(actual->GetValue(c, r).ToString() ==
                expected->GetValue(c, r).ToString());
      }
    }
  }
};

} // namespace

TEST_CASE("exact deltas: single-row UPDATE (autocommit + explicit txn)",
          "[integration][auto_cdc][delta]") {
  DeltaFixture fx;
  auto &m = fx.db.manager();

  uint64_t caps = m.exact_delta_syncs();
  uint64_t scans = m.scan_syncs();
  fx.db.exec("UPDATE wt SET val = 99 WHERE id = 1"); // autocommit
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check_views();

  caps = m.exact_delta_syncs();
  scans = m.scan_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("UPDATE wt SET val = 7 WHERE id = 3");
  fx.db.exec("COMMIT");
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check_views();

  fx.finish();
}

TEST_CASE("exact deltas: multi-row expression UPDATE",
          "[integration][auto_cdc][delta]") {
  DeltaFixture fx;
  auto &m = fx.db.manager();
  const uint64_t caps = m.exact_delta_syncs();
  const uint64_t scans = m.scan_syncs();
  fx.db.exec("UPDATE wt SET val = val + 1 WHERE grp = 1");
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check_views();
  fx.finish();
}

TEST_CASE("exact deltas: UPDATE moves a group-by key",
          "[integration][auto_cdc][delta]") {
  DeltaFixture fx;
  auto &m = fx.db.manager();
  const uint64_t caps = m.exact_delta_syncs();
  fx.db.exec("UPDATE wt SET grp = 2 WHERE id = 1");
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  fx.check_views();
  fx.finish();
}

TEST_CASE("exact deltas: NULL handling in SET and WHERE",
          "[integration][auto_cdc][delta]") {
  DeltaFixture fx;
  auto &m = fx.db.manager();
  uint64_t caps = m.exact_delta_syncs();
  fx.db.exec("UPDATE wt SET val = NULL WHERE id = 2");
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  fx.check_views();

  caps = m.exact_delta_syncs();
  fx.db.exec("UPDATE wt SET val = 5 WHERE val IS NULL");
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  fx.check_views();
  fx.finish();
}

TEST_CASE("exact deltas: DELETE (autocommit + explicit txn + delete-all)",
          "[integration][auto_cdc][delta]") {
  DeltaFixture fx;
  auto &m = fx.db.manager();

  uint64_t caps = m.exact_delta_syncs();
  uint64_t scans = m.scan_syncs();
  fx.db.exec("DELETE FROM wt WHERE id = 4");
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check_views();

  caps = m.exact_delta_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("DELETE FROM wt WHERE grp = 1");
  fx.db.exec("COMMIT");
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  fx.check_views();

  caps = m.exact_delta_syncs();
  fx.db.exec("DELETE FROM wt"); // delete-all, still capturable
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  fx.check_views();
  fx.finish();
}

TEST_CASE("exact deltas: predicate shapes a predictor had to decline",
          "[integration][auto_cdc][delta]") {
  DeltaFixture fx;
  auto &m = fx.db.manager();

  SECTION("DELETE with a subquery WHERE") {
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec(
        "DELETE FROM wt WHERE grp IN (SELECT grp FROM wd WHERE name = 'a')");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    REQUIRE(m.scan_syncs() == scans);
    fx.check_views();
  }
  SECTION("DELETE USING a joined table") {
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("DELETE FROM wt USING wd WHERE wt.grp = wd.grp "
               "AND wd.name = 'b'");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    REQUIRE(m.scan_syncs() == scans);
    fx.check_views();
  }
  SECTION("subquery predicate reading a same-txn write") {
    // the DELETE's subquery must see the uncommitted INSERT into we, so
    // nothing that re-ran the predicate against committed state could get
    // this right (one apply per table: we insert + wt delete), no scan
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("BEGIN");
    fx.db.exec("INSERT INTO we VALUES (3, 'z')");
    fx.db.exec("DELETE FROM wt WHERE id IN (SELECT id FROM we)");
    fx.db.exec("COMMIT");
    REQUIRE(m.exact_delta_syncs() == caps + 2);
    REQUIRE(m.scan_syncs() == scans);
    fx.check_views();
  }
  SECTION("non-deterministic predicate") {
    // random() cannot be re-evaluated to the same answer, so nothing could
    // predict this statement; the trigger reports what it did.
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("UPDATE wt SET val = 1 WHERE random() < 2.0");
    REQUIRE(m.scan_syncs() == scans);
    fx.check_views();
  }
  SECTION("zero matching rows costs one SCOPED scan") {
    // Measured, and a real cost of this source: a write that matches nothing
    // evaluates the ingest scalar zero times, so the commit cannot tell
    // "the trigger fired and nothing changed" from "no trigger fired at all"
    // and reconciles by scan. It is SCOPED to the statement's target (H1),
    // not a full sync_all — that is what is pinned here.
    const uint64_t scans = m.scan_syncs();
    const uint64_t caps = m.exact_delta_syncs();
    fx.db.exec("UPDATE wt SET val = 1 WHERE random() < -1.0");
    REQUIRE(m.scan_syncs() == scans + 1); // wt only, not wd and we
    REQUIRE(m.exact_delta_syncs() == caps);
    fx.check_views();
  }
  fx.finish();
}

TEST_CASE("exact deltas: mixed INSERT+UPDATE+DELETE transaction",
          "[integration][auto_cdc][delta]") {
  DeltaFixture fx;
  auto &m = fx.db.manager();

  SECTION("across different tables: one apply per table") {
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("BEGIN");
    fx.db.exec("INSERT INTO wt VALUES (9, 1, 90)");
    fx.db.exec("UPDATE wd SET name = 'z' WHERE grp = 2");
    fx.db.exec("DELETE FROM we WHERE id = 1");
    fx.db.exec("COMMIT");
    // one apply per touched table, zero scans
    REQUIRE(m.exact_delta_syncs() == caps + 3);
    REQUIRE(m.scan_syncs() == scans);
    fx.check_views();
  }
  SECTION("same table written twice: stays O(delta)") {
    // the UPDATE modifies the row this transaction just INSERTed; both
    // firings buffer into one delta and net to a single appended row
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("BEGIN");
    fx.db.exec("INSERT INTO wt VALUES (9, 1, 90)");
    fx.db.exec("UPDATE wt SET val = 1 WHERE id = 9");
    fx.db.exec("COMMIT");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    REQUIRE(m.scan_syncs() == scans);
    fx.check_views();
  }
  fx.finish();
}

TEST_CASE("exact deltas: rollback discards buffered writes",
          "[integration][auto_cdc][delta]") {
  DeltaFixture fx;
  auto &m = fx.db.manager();
  const uint64_t caps = m.exact_delta_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("UPDATE wt SET val = 1000 WHERE id = 1");
  fx.db.exec("DELETE FROM we WHERE id = 2");
  fx.db.exec("ROLLBACK");
  REQUIRE(m.exact_delta_syncs() == caps);
  fx.check_views(); // views still match (unchanged) base tables
  fx.finish();
}

TEST_CASE("exact deltas: the scan path lands identical views",
          "[integration][auto_cdc][delta]") {
  // The scan-and-diff fallback is still the safety net behind every route
  // out of "I do not know what this transaction wrote", so it has to agree
  // with the trigger path row for row. Same edits, two arms: auto-sync ON
  // (trigger-fed exact deltas) and auto-sync OFF + an explicit dbsp_sync
  // (scan-and-diff). Both are also checked against direct SQL.
  auto run_edits = [](DeltaFixture &fx) {
    fx.db.exec("UPDATE wt SET val = val * 2 WHERE grp = 1");
    fx.db.exec("DELETE FROM wt WHERE id = 3");
    fx.db.exec("BEGIN");
    fx.db.exec("UPDATE wd SET name = 'q' WHERE grp = 1");
    fx.db.exec("COMMIT");
  };

  DeltaFixture exact;
  const uint64_t caps = exact.db.manager().exact_delta_syncs();
  const uint64_t scans = exact.db.manager().scan_syncs();
  run_edits(exact);
  REQUIRE(exact.db.manager().exact_delta_syncs() > caps);
  REQUIRE(exact.db.manager().scan_syncs() == scans);
  exact.check_views();

  DeltaFixture scanned;
  scanned.db.exec("SELECT * FROM dbsp_auto_sync(false)");
  const uint64_t caps2 = scanned.db.manager().exact_delta_syncs();
  const uint64_t scans2 = scanned.db.manager().scan_syncs();
  run_edits(scanned);
  scanned.db.exec("SELECT * FROM dbsp_sync('wt')");
  scanned.db.exec("SELECT * FROM dbsp_sync('wd')");
  REQUIRE(scanned.db.manager().scan_syncs() > scans2);
  REQUIRE(scanned.db.manager().exact_delta_syncs() == caps2);
  scanned.check_views();
  scanned.finish();

  exact.finish();
}

TEST_CASE("exact deltas: notify-API replay matches trigger-fed auto-sync",
          "[integration][auto_cdc][delta]") {
  // Path (a): trigger-fed auto-sync
  DeltaFixture captured;
  captured.db.exec("UPDATE wt SET val = 42 WHERE id = 1");
  captured.db.exec("DELETE FROM wt WHERE id = 3");
  auto a = captured.db.query(
      "SELECT * FROM dbsp_query('wv_agg') ORDER BY ALL");
  REQUIRE_FALSE(a->HasError());
  captured.finish();

  // Path (c): auto-sync off, SQL writes + manual notify replay of the
  // same logical delta (storage first, then notify — the notify contract)
  DeltaFixture notified;
  notified.db.exec("SELECT * FROM dbsp_auto_sync(false)");
  notified.db.exec("UPDATE wt SET val = 42 WHERE id = 1");
  notified.db.exec("DELETE FROM wt WHERE id = 3");
  notified.db.exec("SELECT * FROM dbsp_notify_delete('wt', 1, 1, 10)");
  notified.db.exec("SELECT * FROM dbsp_notify_insert('wt', 1, 1, 42)");
  notified.db.exec("SELECT * FROM dbsp_notify_delete('wt', 3, 2, 30)");
  auto c = notified.db.query(
      "SELECT * FROM dbsp_query('wv_agg') ORDER BY ALL");
  REQUIRE_FALSE(c->HasError());

  REQUIRE(a->RowCount() == c->RowCount());
  for (size_t r = 0; r < a->RowCount(); r++) {
    for (size_t col = 0; col < a->ColumnCount(); col++) {
      REQUIRE(a->GetValue(col, r).ToString() ==
              c->GetValue(col, r).ToString());
    }
  }
}

TEST_CASE("exact deltas: autocommit INSERT VALUES differential",
          "[integration][auto_cdc][delta]") {
  DeltaFixture fx;
  auto &m = fx.db.manager();

  SECTION("multi-row VALUES with expressions and NULLs") {
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("INSERT INTO wt VALUES (10, 1, 5 * 8), (11, 2, NULL)");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    REQUIRE(m.scan_syncs() == scans);
    fx.check_views();
  }
  SECTION("full-cover permuted column list") {
    const uint64_t caps = m.exact_delta_syncs();
    fx.db.exec("INSERT INTO wt (val, id, grp) VALUES (70, 12, 2)");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    fx.check_views();
  }
  SECTION("partial column list, NULL/default padding") {
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("INSERT INTO wt (id, grp) VALUES (13, 1)"); // val -> NULL
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    REQUIRE(m.scan_syncs() == scans);
    fx.check_views();
  }
  SECTION("INSERT ... SELECT differential") {
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("INSERT INTO wt SELECT id + 100, grp, val * 2 FROM wt "
               "WHERE grp = 1");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    REQUIRE(m.scan_syncs() == scans);
    fx.check_views();
  }
  SECTION("volatile expression: the value actually inserted") {
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("INSERT INTO wt VALUES (14, 1, CAST(random() * 0 AS INT))");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    REQUIRE(m.scan_syncs() == scans);
    fx.check_views();
  }
  SECTION("INSERT then UPDATE, separate autocommits") {
    const uint64_t caps = m.exact_delta_syncs();
    fx.db.exec("INSERT INTO wt VALUES (15, 1, 150)");
    fx.db.exec("UPDATE wt SET val = 151 WHERE id = 15");
    REQUIRE(m.exact_delta_syncs() == caps + 2);
    fx.check_views();
  }
  fx.finish();
}

TEST_CASE("Appender rows are captured (flush runs as a statement)",
          "[integration][auto_cdc][appender]") {
  DuckDBTestHarness db;
  db.createTable("wa", "id INT, val INT", {"(1, 10)"});
  db.exec("SELECT * FROM dbsp_track('wa')");
  db.exec("SELECT * FROM dbsp_sync('wa')");
  db.exec("SELECT * FROM dbsp_create_view('wv_app', "
          "'SELECT id, val FROM wa WHERE val > 50')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");
  auto &m = db.manager();

  auto check = [&] {
    auto expected = db.query("SELECT * FROM (SELECT id, val FROM wa "
                             "WHERE val > 50) ORDER BY ALL");
    auto actual =
        db.query("SELECT * FROM dbsp_query('wv_app') ORDER BY ALL");
    REQUIRE_FALSE(expected->HasError());
    REQUIRE_FALSE(actual->HasError());
    REQUIRE(actual->RowCount() == expected->RowCount());
  };

  SECTION("pure Appender transaction: exact, no scan") {
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    db.exec("BEGIN");
    {
      duckdb::Appender app(db.conn(), "wa");
      app.AppendRow(10, 100);
      app.AppendRow(11, 5);
      app.Close();
    }
    db.exec("COMMIT");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    REQUIRE(m.scan_syncs() == scans);
    check();
  }
  SECTION("Appender mixed with a plain INSERT: one apply") {
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    db.exec("BEGIN");
    db.exec("INSERT INTO wa VALUES (20, 200)");
    {
      duckdb::Appender app(db.conn(), "wa");
      app.AppendRow(21, 210);
      app.Close();
    }
    db.exec("COMMIT");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    REQUIRE(m.scan_syncs() == scans);
    check();
  }
  SECTION("Appender after an UPDATE in the same txn: both exact") {
    // Appender::FlushInternal runs an INSERT ... SELECT, so it goes through
    // the binder and fires the INSERT trigger like any other statement —
    // the UPDATE's and the flush's images merge into one delta
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    db.exec("BEGIN");
    db.exec("UPDATE wa SET val = 60 WHERE id = 1");
    {
      duckdb::Appender app(db.conn(), "wa");
      app.AppendRow(30, 300);
      app.Close();
    }
    db.exec("COMMIT");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    REQUIRE(m.scan_syncs() == scans);
    check();
  }
  SECTION("Appender then UPDATE touching its rows: still exact") {
    // the UPDATE modifies rows the flush appended in this same, still-open
    // transaction — the UPDATE trigger reports both images of them
    const uint64_t scans = m.scan_syncs();
    db.exec("BEGIN");
    {
      duckdb::Appender app(db.conn(), "wa");
      app.AppendRow(31, 310);
      app.Close();
    }
    db.exec("UPDATE wa SET val = val + 1 WHERE id = 31");
    db.exec("COMMIT");
    REQUIRE(m.scan_syncs() == scans);
    check();
    auto res = db.query("SELECT val FROM dbsp_query('wv_app') "
                        "WHERE id = 31");
    REQUIRE(res->GetValue(0, 0).GetValue<int64_t>() == 311);
  }
  SECTION("autocommit Appender (no explicit txn): views stay correct") {
    {
      duckdb::Appender app(db.conn(), "wa");
      app.AppendRow(32, 320);
      app.Close();
    }
    check();
  }
  SECTION("Appender rollback discards") {
    db.exec("BEGIN");
    {
      duckdb::Appender app(db.conn(), "wa");
      app.AppendRow(40, 400);
      app.Close();
    }
    db.exec("ROLLBACK");
    check();
    db.assertViewRowCount("wv_app", 0);
  }
  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("dbsp_stats exposes sync-path counters",
          "[integration][auto_cdc][stats]") {
  DuckDBTestHarness db;
  db.createTable("ws", "id INT, val INT", {"(1, 10)"});
  db.exec("SELECT * FROM dbsp_track('ws')");
  db.exec("SELECT * FROM dbsp_sync('ws')");
  db.exec("SELECT * FROM dbsp_create_view('wv_s', "
          "'SELECT id FROM ws WHERE val > 5')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  auto value_of = [&](const std::string &metric) {
    auto res = db.query("SELECT value FROM dbsp_stats() WHERE metric = '" +
                        metric + "'");
    REQUIRE_FALSE(res->HasError());
    REQUIRE(res->RowCount() == 1);
    return res->GetValue(0, 0).GetValue<int64_t>();
  };

  REQUIRE(value_of("tracked_tables") >= 1);
  const auto caps = value_of("exact_delta_syncs");
  const auto trg = value_of("trigger_syncs");
  const auto seq = value_of("commit_seq");
  db.exec("UPDATE ws SET val = 60 WHERE id = 1");
  REQUIRE(value_of("exact_delta_syncs") == caps + 1);
  // The UPDATE trigger fires ONCE but its body evaluates the ingest scalar
  // TWICE — the two arms of the UNION ALL over the old and new transition
  // tables. This is the counter that makes "the triggers are live"
  // verifiable from a host that cannot see C++ state.
  REQUIRE(value_of("trigger_syncs") == trg + 2);
  REQUIRE(value_of("commit_seq") > seq);
  // A retired counter must not come back by accident.
  auto gone = db.query("SELECT count(*) FROM dbsp_stats() WHERE metric IN "
                       "('capture_guard_fallbacks', 'delta_source_mode')");
  REQUIRE(gone->GetValue(0, 0).GetValue<int64_t>() == 0);

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}
