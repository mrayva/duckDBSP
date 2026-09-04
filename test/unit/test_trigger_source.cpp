// Differential oracle for the trigger-fed delta source
// (include/dbsp_trigger_source.hpp), the extension's only change-capture
// mechanism. Views must stay current on a STOCK engine, fed only by the
// generated statement triggers.
//
// The oracle: old images at weight -1, new at +1, update-then-delete of one row
// appears once in the old image and nowhere in the new, insert-then-delete in
// one transaction nets to zero, multi-table transactions apply in one pass,
// rollback discards everything. Every case also cross-checks dbsp_query against
// plain SQL, because a delta source that is self-consistently wrong would pass
// a weight assertion.
//
// It also pins what tracking a table COSTS it: the engine refuses MERGE INTO,
// ON CONFLICT DO UPDATE / INSERT OR REPLACE, and every ALTER but ADD COLUMN on
// a table carrying a trigger. Those are product constraints now, so they are
// asserted rather than discovered.
#include "../test_helpers.hpp"
#include "catch.hpp"
#include "dbsp_trigger_source.hpp"

#include "duckdb/main/appender.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace dbsp_test;

namespace {

// A sum view is the sharpest double-count detector available: a row ingested
// twice doubles the sum, where a row count would only show it if the view kept
// duplicates.
double view_sum(DuckDBTestHarness &db, const std::string &view) {
  auto rows = db.getViewRows(view);
  // SUM over no rows is NULL, not 0 — which is what an emptied table gives.
  if (rows.empty() || rows[0][0].IsNull()) {
    return 0.0;
  }
  return rows[0][0].GetValue<double>();
}

double sql_sum(DuckDBTestHarness &db, const std::string &sql) {
  auto r = db.query(sql);
  REQUIRE_FALSE(r->HasError());
  auto v = r->GetValue(0, 0);
  return v.IsNull() ? 0.0 : v.GetValue<double>();
}

int64_t sql_count(DuckDBTestHarness &db, const std::string &sql) {
  auto r = db.query(sql);
  REQUIRE_FALSE(r->HasError());
  return r->GetValue(0, 0).GetValue<int64_t>();
}

} // namespace

TEST_CASE("trigger source: tracking a table installs its triggers",
          "[trigger_source]") {
  DuckDBTestHarness db;
  db.createTable("items", "id INTEGER, name VARCHAR, price DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('items')");
  db.exec("SELECT * FROM dbsp_create_view('all_items', 'SELECT * FROM items')");

  // Three triggers on the tracked table, plus the sink the bodies write into.
  auto trg = db.query("SELECT trigger_name FROM duckdb_triggers() ORDER BY 1");
  REQUIRE_FALSE(trg->HasError());
  REQUIRE(trg->RowCount() == 3);
  REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_tables() WHERE table_name "
                        "= 'dbsp_trigger_sink'") == 1);
}

TEST_CASE("trigger source: insert/update/delete keep the view current",
          "[trigger_source]") {
  DuckDBTestHarness db;
  db.createTable("items", "id INTEGER, name VARCHAR, price DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('items')");
  db.exec("SELECT * FROM dbsp_create_view('expensive', "
          "'SELECT * FROM items WHERE price > 50')");
  db.exec("SELECT * FROM dbsp_create_view('total', "
          "'SELECT SUM(price) AS s FROM items')");

  const auto syncs_before =
      dbsp_native::trigger_source_stats().trigger_syncs.load();

  db.exec("INSERT INTO items VALUES (1, 'widget', 10.0), (2, 'gadget', 100.0)");
  auto rows = db.getViewRows("expensive");
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0][1].ToString() == "gadget");
  REQUIRE(view_sum(db, "total") == sql_sum(db, "SELECT SUM(price) FROM items"));

  db.exec("UPDATE items SET price = 60.0 WHERE id = 1");
  REQUIRE(db.getViewRows("expensive").size() == 2);
  REQUIRE(view_sum(db, "total") == sql_sum(db, "SELECT SUM(price) FROM items"));

  db.exec("DELETE FROM items WHERE id = 2");
  rows = db.getViewRows("expensive");
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0][1].ToString() == "widget");
  REQUIRE(view_sum(db, "total") == sql_sum(db, "SELECT SUM(price) FROM items"));

  // PROOF OF LIFE: these commits were served by trigger bodies, and the flag
  // that lets the commit path trust them flipped as a result.
  REQUIRE(dbsp_native::trigger_source_stats().trigger_syncs.load() >
          syncs_before);
  REQUIRE(dbsp_native::trigger_source_active());
}

TEST_CASE("trigger source: one insert counts exactly once (no double count)",
          "[trigger_source]") {
  // Each firing must deliver its rows exactly ONCE. A second delivery would
  // double the sum; the row count alone would not show it, because the view
  // would still hold one distinct row.
  DuckDBTestHarness db;
  db.createTable("nums", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('nums')");
  db.exec("SELECT * FROM dbsp_create_view('total', "
          "'SELECT SUM(v) AS s FROM nums')");
  db.exec("SELECT * FROM dbsp_create_view('cnt', "
          "'SELECT COUNT(*) AS c FROM nums')");

  db.exec("INSERT INTO nums VALUES (1, 5.0)");
  REQUIRE(view_sum(db, "total") == 5.0);
  REQUIRE(db.getViewRows("cnt")[0][0].GetValue<int64_t>() == 1);

  db.exec("INSERT INTO nums VALUES (2, 5.0)");
  REQUIRE(view_sum(db, "total") == 10.0);
  REQUIRE(db.getViewRows("cnt")[0][0].GetValue<int64_t>() == 2);

  db.exec("UPDATE nums SET v = 1.0 WHERE id = 1");
  REQUIRE(view_sum(db, "total") == 6.0);
  REQUIRE(db.getViewRows("cnt")[0][0].GetValue<int64_t>() == 2);
}

TEST_CASE("trigger source: multi-statement single transaction",
          "[trigger_source]") {
  DuckDBTestHarness db;
  db.createTable("items", "id INTEGER, name VARCHAR, price DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('items')");
  db.exec("SELECT * FROM dbsp_create_view('expensive', "
          "'SELECT * FROM items WHERE price > 50')");
  db.exec("SELECT * FROM dbsp_create_view('total', "
          "'SELECT SUM(price) AS s FROM items')");

  SECTION("mixed DML in one commit") {
    db.exec("INSERT INTO items VALUES (1,'a',80.0),(2,'b',90.0),(3,'c',20.0)");
    db.exec("BEGIN TRANSACTION");
    db.exec("INSERT INTO items VALUES (4, 'd', 200.0)");
    db.exec("UPDATE items SET price = 55.0 WHERE id = 3");
    db.exec("DELETE FROM items WHERE id = 1");
    db.exec("COMMIT");
    // survivors > 50: (2,b,90), (3,c,55), (4,d,200)
    REQUIRE(db.getViewRows("expensive").size() == 3);
    REQUIRE(view_sum(db, "total") ==
            sql_sum(db, "SELECT SUM(price) FROM items"));
  }

  SECTION("insert then delete the same row nets to zero") {
    db.exec("INSERT INTO items VALUES (1, 'keep', 70.0)");
    const double before = view_sum(db, "total");
    db.exec("BEGIN TRANSACTION");
    db.exec("INSERT INTO items VALUES (2, 'ghost', 500.0)");
    db.exec("DELETE FROM items WHERE id = 2");
    db.exec("COMMIT");
    REQUIRE(view_sum(db, "total") == before);
    REQUIRE(db.getViewRows("expensive").size() == 1);
    REQUIRE(view_sum(db, "total") ==
            sql_sum(db, "SELECT SUM(price) FROM items"));
  }

  SECTION("update chain collapses to first-old / last-new") {
    db.exec("INSERT INTO items VALUES (1, 'x', 10.0)");
    db.exec("BEGIN TRANSACTION");
    db.exec("UPDATE items SET price = 60.0 WHERE id = 1");
    db.exec("UPDATE items SET price = 70.0 WHERE id = 1");
    db.exec("UPDATE items SET price = 80.0 WHERE id = 1");
    db.exec("COMMIT");
    REQUIRE(db.getViewRows("expensive").size() == 1);
    REQUIRE(view_sum(db, "total") == 80.0);
    REQUIRE(view_sum(db, "total") ==
            sql_sum(db, "SELECT SUM(price) FROM items"));
  }

  SECTION("update then delete of the same row in one txn") {
    db.exec("INSERT INTO items VALUES (1, 'x', 60.0), (2, 'y', 70.0)");
    db.exec("BEGIN TRANSACTION");
    db.exec("UPDATE items SET price = 999.0 WHERE id = 1");
    db.exec("DELETE FROM items WHERE id = 1");
    db.exec("COMMIT");
    auto rows = db.getViewRows("expensive");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][1].ToString() == "y");
    REQUIRE(view_sum(db, "total") ==
            sql_sum(db, "SELECT SUM(price) FROM items"));
  }
}

TEST_CASE("trigger source: explicit rollback discards everything",
          "[trigger_source]") {
  DuckDBTestHarness db;
  db.createTable("items", "id INTEGER, name VARCHAR, price DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('items')");
  db.exec("SELECT * FROM dbsp_create_view('expensive', "
          "'SELECT * FROM items WHERE price > 50')");
  db.exec("SELECT * FROM dbsp_create_view('total', "
          "'SELECT SUM(price) AS s FROM items')");

  db.exec("INSERT INTO items VALUES (1, 'keep', 70.0)");
  db.exec("BEGIN TRANSACTION");
  db.exec("INSERT INTO items VALUES (2, 'phantom', 500.0)");
  db.exec("UPDATE items SET price = 5.0 WHERE id = 1");
  db.exec("ROLLBACK");

  auto rows = db.getViewRows("expensive");
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0][1].ToString() == "keep");
  REQUIRE(view_sum(db, "total") == 70.0);
  REQUIRE(view_sum(db, "total") == sql_sum(db, "SELECT SUM(price) FROM items"));
}

TEST_CASE("trigger source: multi-table one-transaction commit",
          "[trigger_source]") {
  DuckDBTestHarness db;
  db.createTable("a", "id INTEGER, v DOUBLE", {});
  db.createTable("b", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('a')");
  db.exec("SELECT * FROM dbsp_track('b')");
  db.exec("SELECT * FROM dbsp_create_view('sum_a', 'SELECT SUM(v) AS s FROM a')");
  db.exec("SELECT * FROM dbsp_create_view('sum_b', 'SELECT SUM(v) AS s FROM b')");

  db.exec("BEGIN TRANSACTION");
  db.exec("INSERT INTO a VALUES (1, 10.0), (2, 20.0)");
  db.exec("INSERT INTO b VALUES (1, 100.0)");
  db.exec("COMMIT");

  // Both tables in ONE commit: a per-table apply would keep only the last
  // table's downstream deltas.
  REQUIRE(view_sum(db, "sum_a") == 30.0);
  REQUIRE(view_sum(db, "sum_b") == 100.0);

  db.exec("BEGIN TRANSACTION");
  db.exec("DELETE FROM a WHERE id = 1");
  db.exec("UPDATE b SET v = 7.0 WHERE id = 1");
  db.exec("COMMIT");
  REQUIRE(view_sum(db, "sum_a") == 20.0);
  REQUIRE(view_sum(db, "sum_b") == 7.0);
  REQUIRE(view_sum(db, "sum_a") == sql_sum(db, "SELECT SUM(v) FROM a"));
  REQUIRE(view_sum(db, "sum_b") == sql_sum(db, "SELECT SUM(v) FROM b"));
}

TEST_CASE("trigger source: the C++ Appender updates the view",
          "[trigger_source]") {
  // The Appender used to be the write path the binder never saw, which is
  // what forced every earlier delta source to keep a pessimistic fallback.
  // In 2.0 Appender::FlushInternal runs an INSERT ... SELECT, so the binder —
  // and therefore the trigger — does see it.
  DuckDBTestHarness db;
  db.createTable("nums", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('nums')");
  db.exec("SELECT * FROM dbsp_create_view('total', "
          "'SELECT SUM(v) AS s FROM nums')");
  db.exec("INSERT INTO nums VALUES (0, 1.0)"); // arms the triggers

  {
    duckdb::Appender appender(db.conn(), "nums");
    for (int i = 1; i <= 3; i++) {
      appender.BeginRow();
      appender.Append<int32_t>(i);
      appender.Append<double>(10.0 * i);
      appender.EndRow();
    }
    appender.Close();
  }

  REQUIRE(sql_sum(db, "SELECT SUM(v) FROM nums") == 61.0);
  REQUIRE(view_sum(db, "total") == 61.0);
}

TEST_CASE("trigger source: COPY FROM a CSV updates the view",
          "[trigger_source]") {
  DuckDBTestHarness db;
  db.createTable("nums", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('nums')");
  db.exec("SELECT * FROM dbsp_create_view('total', "
          "'SELECT SUM(v) AS s FROM nums')");
  db.exec("INSERT INTO nums VALUES (0, 1.0)"); // arms the triggers

  const std::string csv = "dbsp_trigger_source_probe.csv";
  {
    std::ofstream out(csv);
    out << "5,50.0\n6,60.0\n";
  }
  db.exec("COPY nums FROM '" + csv + "' (FORMAT CSV, HEADER FALSE)");
  std::remove(csv.c_str());

  REQUIRE(sql_sum(db, "SELECT SUM(v) FROM nums") == 111.0);
  REQUIRE(view_sum(db, "total") == 111.0);
}

TEST_CASE("trigger source: MERGE INTO on a tracked table is rejected",
          "[trigger_source]") {
  // Pinned, not worked around: the engine refuses MERGE INTO on ANY table with
  // a trigger (bind_merge_into.cpp). Tracking a table in trigger mode
  // therefore REMOVES a working DuckDB feature from it — the one real price of
  // this source, and the reason it cannot simply replace the others.
  DuckDBTestHarness db;
  db.createTable("target", "id INTEGER, v DOUBLE", {});
  db.createTable("src", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('target')");
  db.exec("SELECT * FROM dbsp_create_view('total', "
          "'SELECT SUM(v) AS s FROM target')");
  db.exec("INSERT INTO target VALUES (1, 1.0)"); // arms the triggers

  auto r = db.query("MERGE INTO target USING src ON target.id = src.id "
                    "WHEN MATCHED THEN UPDATE SET v = src.v");
  REQUIRE(r->HasError());
  REQUIRE(r->GetError().find("MERGE INTO is not supported on tables with "
                             "triggers") != std::string::npos);
}

TEST_CASE("trigger source: untracked tables are ignored", "[trigger_source]") {
  DuckDBTestHarness db;
  db.createTable("tracked", "id INTEGER, v DOUBLE", {});
  db.createTable("untracked", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('tracked')");
  db.exec("SELECT * FROM dbsp_create_view('big', "
          "'SELECT * FROM tracked WHERE v > 1')");

  // Only the tracked table carries triggers.
  auto trg = db.query("SELECT count(*) FROM duckdb_triggers()");
  REQUIRE_FALSE(trg->HasError());
  REQUIRE(trg->GetValue(0, 0).GetValue<int64_t>() == 3);

  db.exec("INSERT INTO untracked VALUES (1, 100.0)");
  db.exec("INSERT INTO tracked VALUES (1, 2.0)");
  REQUIRE(db.getViewRows("big").size() == 1);
}

TEST_CASE("trigger source: NULLs survive the round trip", "[trigger_source]") {
  // The ingest scalar declares SPECIAL_NULL_HANDLING; default handling would
  // have quietly dropped every row carrying a NULL.
  DuckDBTestHarness db;
  db.createTable("t", "id INTEGER, name VARCHAR, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_create_view('nulls', "
          "'SELECT COUNT(*) AS c FROM t WHERE name IS NULL')");
  db.exec("SELECT * FROM dbsp_create_view('cnt', 'SELECT COUNT(*) AS c FROM t')");

  db.exec("INSERT INTO t VALUES (1, NULL, NULL), (2, 'x', 1.0), (3, NULL, 2.0)");
  REQUIRE(db.getViewRows("cnt")[0][0].GetValue<int64_t>() == 3);
  REQUIRE(db.getViewRows("nulls")[0][0].GetValue<int64_t>() == 2);

  db.exec("DELETE FROM t WHERE name IS NULL");
  REQUIRE(db.getViewRows("cnt")[0][0].GetValue<int64_t>() == 1);
  REQUIRE(db.getViewRows("nulls")[0][0].GetValue<int64_t>() == 0);
}

// ---------------------------------------------------------------------------
// Fix round 1: the cases the review found. Each of these FAILED before the
// schema-keyed reconcile and the scalar's mode guard landed.
// ---------------------------------------------------------------------------

TEST_CASE("trigger source: ALTER TABLE ADD COLUMN regenerates the bodies",
          "[trigger_source]") {
  // C1. A trigger body pins its table's column list at generation time, and
  // the old sweep skipped whenever the tracked-table COUNT was unchanged —
  // which an ALTER never changes. The body kept emitting the old width, the
  // new column arrived NULL, and the commit took the exact-delta fast path
  // with no scan to catch it. Measured before the fix: view 5.0, SQL 12.0.
  DuckDBTestHarness db;
  db.createTable("t", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");
  db.exec("INSERT INTO t VALUES (1, 5.0)");
  REQUIRE(view_sum(db, "tot") == 5.0);

  db.exec("ALTER TABLE t ADD COLUMN note VARCHAR");
  db.exec("INSERT INTO t VALUES (2, 7.0, 'hello')");

  REQUIRE(sql_sum(db, "SELECT SUM(v) FROM t") == 12.0);
  REQUIRE(view_sum(db, "tot") == 12.0);

  // and the added column is really carried, not silently NULL
  db.exec("SELECT * FROM dbsp_create_view('byw', "
          "'SELECT note, COUNT(*) AS c FROM t GROUP BY note')");
  db.exec("INSERT INTO t VALUES (3, 1.0, 'world')");
  auto rows = db.getViewRows("byw");
  int64_t named = 0;
  for (auto &r : rows) {
    if (!r[0].IsNull() && r[0].ToString() == "world") {
      named = r[1].GetValue<int64_t>();
    }
  }
  REQUIRE(named == 1);
  REQUIRE(sql_count(db, "SELECT COUNT(*) FROM t WHERE note = 'world'") == 1);
}

TEST_CASE("trigger source: DROP TABLE and recreate gets its triggers back",
          "[trigger_source]") {
  // I1. DROP TABLE takes the triggers with it (verified against the engine),
  // and the count-equality shortcut meant they were never reinstalled: the
  // table stayed tracked and permanently triggerless, so every commit fell
  // through to a full scan. Views stayed right; incrementality was silently
  // gone.
  DuckDBTestHarness db;
  db.createTable("t", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");
  db.exec("INSERT INTO t VALUES (1, 5.0)");
  REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 3);

  db.exec("DROP TABLE t");
  REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 0);
  db.exec("CREATE TABLE t (id INTEGER, v DOUBLE)");
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_create_view('tot2', 'SELECT SUM(v) AS s FROM t')");

  REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 3);
  db.exec("INSERT INTO t VALUES (9, 4.0)");
  REQUIRE(view_sum(db, "tot2") == sql_sum(db, "SELECT SUM(v) FROM t"));
}

TEST_CASE("trigger source: upsert forms are rejected on a tracked table",
          "[trigger_source]") {
  // I2. `MERGE INTO` is NOT the only thing tracking costs: the engine refuses
  // ON CONFLICT DO UPDATE and INSERT OR REPLACE on a table carrying a
  // NEW-TABLE trigger. Both work in default and capture mode. Pinned with the
  // engine's own wording so a future engine that lifts it says so loudly.
  DuckDBTestHarness db;
  db.createTable("t", "id INTEGER PRIMARY KEY, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");
  db.exec("INSERT INTO t VALUES (1, 1.0)");

  auto r1 = db.query("INSERT INTO t VALUES (1, 2.0) "
                     "ON CONFLICT (id) DO UPDATE SET v = 2.0");
  REQUIRE(r1->HasError());
  REQUIRE(r1->GetError().find("ON CONFLICT DO UPDATE is not yet supported") !=
          std::string::npos);

  auto r2 = db.query("INSERT OR REPLACE INTO t VALUES (1, 3.0)");
  REQUIRE(r2->HasError());
  REQUIRE(r2->GetError().find("ON CONFLICT DO UPDATE is not yet supported") !=
          std::string::npos);

  // DO NOTHING has no such restriction, and must keep working
  db.exec("INSERT INTO t VALUES (1, 9.0) ON CONFLICT DO NOTHING");
  REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));
}

TEST_CASE("trigger source: schema-changing ALTERs are refused by the engine",
          "[trigger_source]") {
  // I3. Everything except ADD COLUMN throws on a table carrying a trigger,
  // because the trigger is a catalog dependency. All four succeed in default
  // mode. Documented in DESIGN_TRIGGER_SOURCE.md; one form pinned here.
  DuckDBTestHarness db;
  db.createTable("t", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");
  db.exec("INSERT INTO t VALUES (1, 1.0)");

  auto r = db.query("ALTER TABLE t RENAME COLUMN v TO w");
  REQUIRE(r->HasError());
  REQUIRE(r->GetError().find("because there are entries that depend on it") !=
          std::string::npos);
}

TEST_CASE("trigger source: INSERT ... SELECT and TRUNCATE", "[trigger_source]") {
  // Both were measured by a throwaway shell probe and never pinned.
  DuckDBTestHarness db;
  db.createTable("t", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");
  db.exec("SELECT * FROM dbsp_create_view('cnt', 'SELECT COUNT(*) AS c FROM t')");
  db.exec("INSERT INTO t VALUES (1, 1.0), (2, 2.0)");

  db.exec("INSERT INTO t SELECT id + 10, v * 10 FROM t");
  REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));
  REQUIRE(db.getViewRows("cnt")[0][0].GetValue<int64_t>() == 4);

  db.exec("TRUNCATE t");
  REQUIRE(sql_count(db, "SELECT COUNT(*) FROM t") == 0);
  REQUIRE(db.getViewRows("cnt")[0][0].GetValue<int64_t>() == 0);
  REQUIRE(view_sum(db, "tot") == 0.0);
}

TEST_CASE("trigger source: multi-chunk DML under parallelism",
          "[trigger_source]") {
  // Bodies run on worker threads and an aggregate over a big transition table
  // may be parallel, so the buffer takes a mutex. >1 chunk (2048 rows) with
  // threads=8 is the shape that would expose a lost or doubled chunk.
  DuckDBTestHarness db;
  db.exec("SET threads=8");
  db.createTable("t", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");
  db.exec("SELECT * FROM dbsp_create_view('cnt', 'SELECT COUNT(*) AS c FROM t')");
  db.exec("INSERT INTO t VALUES (0, 1.0)"); // arms the triggers

  db.exec("INSERT INTO t SELECT i, 1.0 FROM range(1, 20001) tbl(i)");
  REQUIRE(db.getViewRows("cnt")[0][0].GetValue<int64_t>() == 20001);
  REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));

  db.exec("UPDATE t SET v = 2.0 WHERE id % 2 = 0");
  REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));

  db.exec("DELETE FROM t WHERE id > 10000");
  REQUIRE(db.getViewRows("cnt")[0][0].GetValue<int64_t>() ==
          sql_count(db, "SELECT COUNT(*) FROM t"));
  REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));
}

TEST_CASE("trigger source: a user's own trigger coexists", "[trigger_source]") {
  // Tracking must not disturb a trigger the user already had, and the user's
  // trigger must not be mistaken for one of ours by the reconcile.
  DuckDBTestHarness db;
  db.createTable("t", "id INTEGER, v DOUBLE", {});
  db.createTable("audit", "id INTEGER", {});
  db.exec("CREATE TRIGGER user_audit AFTER INSERT ON t "
          "REFERENCING NEW TABLE AS n FOR EACH STATEMENT "
          "INSERT INTO audit SELECT id FROM n");
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");

  db.exec("INSERT INTO t VALUES (1, 5.0), (2, 6.0)");
  REQUIRE(view_sum(db, "tot") == 11.0);
  REQUIRE(sql_count(db, "SELECT COUNT(*) FROM audit") == 2);
  REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 4);
}

// ---------------------------------------------------------------------------
// Fix round 2: DDL inside an explicit transaction. The round-1 sweep read the
// caller's catalog but ran its DDL on an internal connection, which cannot see
// uncommitted catalog changes — so it threw out of the user's own COMMIT, and
// a CREATE + track in one transaction wedged the connection permanently.
// ---------------------------------------------------------------------------

TEST_CASE("trigger source: ADD COLUMN inside an explicit transaction",
          "[trigger_source]") {
  DuckDBTestHarness db;
  db.createTable("t", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");
  db.exec("INSERT INTO t VALUES (1, 5.0)");

  SECTION("ALTER alone in the transaction") {
    // Measured before this round: the COMMIT itself raised
    // "DBSP trigger source: Binder Error: Referenced column \"note\" not found"
    // and left the transaction open.
    db.exec("BEGIN TRANSACTION");
    db.exec("ALTER TABLE t ADD COLUMN note VARCHAR");
    db.exec("COMMIT"); // must not throw

    db.exec("SELECT * FROM dbsp_create_view('byw', "
            "'SELECT note, COUNT(*) AS c FROM t GROUP BY note')");
    db.exec("INSERT INTO t VALUES (2, 7.0, 'world')");
    REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));
    REQUIRE(sql_count(db, "SELECT COUNT(*) FROM t WHERE note = 'world'") == 1);
    int64_t named = 0;
    for (auto &r : db.getViewRows("byw")) {
      if (!r[0].IsNull() && r[0].ToString() == "world") {
        named = r[1].GetValue<int64_t>();
      }
    }
    REQUIRE(named == 1);
  }

  SECTION("ALTER and a write in the SAME transaction") {
    // The write runs under bodies that no longer match the table, so its
    // commit must reconcile by scan — and the NEXT commit, after the bodies
    // are regenerated, must be exact again.
    db.exec("BEGIN TRANSACTION");
    db.exec("ALTER TABLE t ADD COLUMN note VARCHAR");
    db.exec("INSERT INTO t VALUES (2, 7.0, 'a')");
    db.exec("COMMIT");
    REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));

    const auto trg0 = dbsp_native::trigger_source_stats().trigger_syncs.load();
    const auto scan0 = db.manager().scan_syncs();
    db.exec("INSERT INTO t VALUES (3, 2.0, 'b')");
    REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));
    REQUIRE(dbsp_native::trigger_source_stats().trigger_syncs.load() > trg0);
    REQUIRE(db.manager().scan_syncs() == scan0); // exact again, no scan
  }

  SECTION("ALTER rolled back leaves the bodies alone") {
    db.exec("BEGIN TRANSACTION");
    db.exec("ALTER TABLE t ADD COLUMN note VARCHAR");
    db.exec("ROLLBACK");
    REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 3);

    const auto trg0 = dbsp_native::trigger_source_stats().trigger_syncs.load();
    const auto scan0 = db.manager().scan_syncs();
    db.exec("INSERT INTO t VALUES (2, 7.0)"); // still two columns
    REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));
    REQUIRE(dbsp_native::trigger_source_stats().trigger_syncs.load() > trg0);
    REQUIRE(db.manager().scan_syncs() == scan0);
  }
}

TEST_CASE("trigger source: CREATE and track inside one transaction",
          "[trigger_source]") {
  // Measured before this round: every statement after the track — the INSERT,
  // the COMMIT, the ROLLBACK, and a bare SELECT 1 — threw
  // "DBSP trigger source: Catalog Error: Table with name u does not exist!",
  // and only closing the connection escaped it.
  SECTION("committed") {
    DuckDBTestHarness db;
    db.exec("BEGIN TRANSACTION");
    db.exec("CREATE TABLE u (id INTEGER, v DOUBLE)");
    db.exec("SELECT * FROM dbsp_track('u')");
    db.exec("INSERT INTO u VALUES (1, 5.0)");
    db.exec("COMMIT");

    db.exec("SELECT * FROM dbsp_create_view('tu', 'SELECT SUM(v) AS s FROM u')");
    db.exec("INSERT INTO u VALUES (2, 3.0)");
    REQUIRE(view_sum(db, "tu") == sql_sum(db, "SELECT SUM(v) FROM u"));
    REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 3);
  }

  SECTION("rolled back") {
    DuckDBTestHarness db;
    db.exec("BEGIN TRANSACTION");
    db.exec("CREATE TABLE u (id INTEGER, v DOUBLE)");
    db.exec("SELECT * FROM dbsp_track('u')");
    db.exec("INSERT INTO u VALUES (1, 5.0)");
    db.exec("ROLLBACK");

    // The table never existed; nothing may throw on the way out, and no
    // triggers may be left behind for it.
    db.exec("SELECT 1");
    REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 0);
    db.exec("SELECT 42");

    // The TRACKING INTENT must roll back with the table. DBSP's tracked-table
    // set is process state, not transactional state, so it has to be dropped
    // by hand (DBSPContextState::TransactionRollback). Left standing,
    // dbsp_tables() went on listing a table that never committed, and the next
    // table to take the name `u` — of any shape — would silently be handed
    // trigger bodies and an empty baseline.
    REQUIRE(sql_count(db, "SELECT count(*) FROM dbsp_tables() "
                          "WHERE table_name LIKE '%.u'") == 0);

    // and the connection is still usable for real work
    db.createTable("w", "id INTEGER, v DOUBLE", {});
    db.exec("SELECT * FROM dbsp_track('w')");
    db.exec("SELECT * FROM dbsp_create_view('tw', 'SELECT SUM(v) AS s FROM w')");
    db.exec("INSERT INTO w VALUES (1, 4.0)");
    REQUIRE(view_sum(db, "tw") == 4.0);
  }
}

TEST_CASE("trigger source: a triggered catalog can still be DETACHed",
          "[trigger_source]") {
  // Reading a catalog's version (the sweep's gate) calls Transaction::Get for
  // that catalog, which JOINS it to the statement's transaction — and DETACH
  // refuses to run when the transaction has outstanding work on the target
  // (duckdb/src/execution/operator/schema/physical_detach.cpp:20-30).
  //
  // Measured before the fix: `DETACH m` threw "Cannot detach database m
  // because the current transaction has outstanding work on it - commit or
  // rollback first" on a connection that had only ever read from m. NumPad
  // attaches a database per model, so this was a hard blocker, not a corner.
  DuckDBTestHarness db;
  const std::string path =
      std::string(std::tmpnam(nullptr)) + "_dbsp_detach.duckdb";
  {
    duckdb::DuckDB other(path);
    duckdb::Connection setup(other);
    REQUIRE_FALSE(setup.Query("CREATE TABLE li (k INTEGER, v DOUBLE)")->HasError());
    REQUIRE_FALSE(
        setup.Query("INSERT INTO li VALUES (0, 1.0), (1, 2.0)")->HasError());
  }
  db.exec("ATTACH '" + path + "' AS m (READ_WRITE)");
  db.exec("SELECT * FROM dbsp_track('m.li')");
  db.exec("SELECT * FROM dbsp_create_view('m_sum', "
          "'SELECT SUM(v) AS s FROM m.li')");
  db.exec("INSERT INTO m.li VALUES (2, 4.0)");
  REQUIRE(view_sum(db, "m_sum") == sql_sum(db, "SELECT SUM(v) FROM m.li"));

  // The statements above have all run the sweep against catalog m.
  auto res = db.query("DETACH m");
  INFO("DETACH: " << (res->HasError() ? res->GetError() : std::string("ok")));
  REQUIRE_FALSE(res->HasError());
  std::remove(path.c_str());
}

TEST_CASE("trigger source: an empty install record re-uses the catalog's bodies",
          "[trigger_source]") {
  // A fresh process (or a reopened database) starts with NO per-database
  // install record while the trigger bodies are already in the catalog, since
  // triggers are catalog objects that outlive any one process.
  //
  // Re-issuing CREATE OR REPLACE there is not free: the new bodies commit on
  // an internal connection AFTER the current statement took its catalog
  // snapshot, so the sweep has to mark that transaction untrusted and its
  // commit reconciles by scanning EVERY tracked table. Doing that on the first
  // statement after every reopen is what fingerprinted trigger names exist to
  // avoid — the names encode the column fingerprint, so their presence alone
  // proves the bodies are current.
  //
  // dbsp_forget_triggers() is exactly what a process restart does to that
  // record, which is what makes this testable in one process.
  DuckDBTestHarness db;
  db.createTable("t", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");
  db.exec("INSERT INTO t VALUES (1, 5.0)");
  REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 3);

  dbsp_native::dbsp_forget_triggers(
      static_cast<const void *>(db.conn().context->db.get()));

  auto &ctx = *db.conn().context;
  dbsp_native::ReconcileResult result =
      dbsp_native::ReconcileResult::DEFERRED;
  ctx.RunFunctionInTransaction([&] {
    result = dbsp_native::install_pending_triggers(
        ctx, dbsp_native::get_cdc_manager(ctx));
  });
  REQUIRE(result == dbsp_native::ReconcileResult::UNCHANGED);
  REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 3);

  // and the source still works afterwards
  db.exec("INSERT INTO t VALUES (2, 7.0)");
  REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));
}

TEST_CASE("trigger source: bodies lost without a fingerprint change come back",
          "[trigger_source]") {
  // A table can LOSE its triggers while its column fingerprint and the
  // tracked-table count both stand still. The sweep used to short-circuit on
  // its own install record in exactly that case and never look at the catalog
  // again, so the table stayed triggerless for the life of the process: every
  // later write to it paid a scoped scan, forever, with the view still exact
  // and no error anywhere — the silent-staleness outcome this source is not
  // allowed to have.

  SECTION("DROP + CREATE of the same shape inside one transaction") {
    // DROP TABLE takes the bodies with it. Re-creating the SAME columns in the
    // same transaction leaves the fingerprint identical and the tracked count
    // unmoved, so nothing but the catalog itself can tell.
    DuckDBTestHarness db;
    db.createTable("t", "id INTEGER, v DOUBLE", {"(1, 5.0)"});
    db.exec("SELECT * FROM dbsp_track('t')");
    db.exec(
        "SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");
    db.exec("INSERT INTO t VALUES (2, 7.0)"); // arms the triggers
    REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 3);

    db.exec("BEGIN TRANSACTION");
    db.exec("DROP TABLE t");
    db.exec("CREATE TABLE t (id INTEGER, v DOUBLE)");
    db.exec("COMMIT");
    db.exec("SELECT 1"); // the sweep's first look after the transaction ended
    REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 3);

    // and the next write is served by an exact delta, not a scan
    db.exec("SELECT * FROM dbsp_sync('t')");
    const auto scans = db.manager().scan_syncs();
    const auto caps = db.manager().captured_delta_syncs();
    db.exec("INSERT INTO t VALUES (3, 11.0)");
    REQUIRE(db.manager().scan_syncs() == scans);
    REQUIRE(db.manager().captured_delta_syncs() == caps + 1);
    REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));
  }

  SECTION("a single body dropped by hand is restored") {
    DuckDBTestHarness db;
    db.createTable("t", "id INTEGER, v DOUBLE", {});
    db.exec("SELECT * FROM dbsp_track('t')");
    db.exec(
        "SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");
    db.exec("INSERT INTO t VALUES (1, 5.0)");
    auto names = db.query("SELECT trigger_name FROM duckdb_triggers() "
                          "WHERE trigger_name LIKE '%\\_del' ESCAPE '\\'");
    REQUIRE(names->RowCount() == 1);
    db.exec("DROP TRIGGER " + names->GetValue(0, 0).ToString() + " ON t");
    REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 2);

    db.exec("SELECT 1"); // the sweep sees the catalog version moved
    REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 3);

    const auto scans = db.manager().scan_syncs();
    db.exec("DELETE FROM t WHERE id = 1");
    REQUIRE(db.manager().scan_syncs() == scans);
    REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));
  }
}

TEST_CASE("trigger source: tracking refuses a pre-v2.0.0 database, loudly",
          "[trigger_source]") {
  // CREATE TRIGGER needs storage version v2.0.0 or higher. Without a precheck
  // the refusal arrived from the SWEEP, i.e. at the QueryBegin of some later
  // statement — so dbsp_track SUCCEEDED, the table stayed in the tracked set,
  // and every statement on the connection afterwards (reads included) threw
  // the engine's message until the connection was closed.
  DuckDBTestHarness db;
  const std::string path =
      std::string(std::tmpnam(nullptr)) + "_dbsp_v1.duckdb";
  db.exec("ATTACH '" + path + "' AS old (STORAGE_VERSION 'v1.0.0')");
  db.exec("CREATE TABLE old.t (id INTEGER, v DOUBLE)");
  db.exec("INSERT INTO old.t VALUES (1, 5.0)");

  auto res = db.query("SELECT * FROM dbsp_track('old.t')");
  REQUIRE(res->HasError());
  const std::string err = res->GetError();
  INFO(err);
  // ONE readable error that names the version it found and the way out.
  REQUIRE(err.find("storage version") != std::string::npos);
  REQUIRE(err.find("v1.0.0") != std::string::npos);
  REQUIRE(err.find("STORAGE_VERSION 'v2.0.0'") != std::string::npos);
  REQUIRE(err.find("COPY FROM DATABASE") != std::string::npos);
  REQUIRE(err.find("dbsp_spill") != std::string::npos);

  // The table must NOT be tracked, and the connection must still be usable —
  // that pair is the whole point of prechecking instead of letting the sweep
  // throw later.
  REQUIRE(sql_count(db, "SELECT count(*) FROM dbsp_tables() "
                        "WHERE table_name LIKE '%.t'") == 0);
  REQUIRE(sql_count(db, "SELECT 1") == 1);
  REQUIRE(sql_count(db, "SELECT count(*) FROM old.t") == 1);

  // A v2.0.0 catalog on the same connection still tracks normally.
  db.createTable("fresh", "id INTEGER, v DOUBLE", {"(1, 3.0)"});
  db.exec("SELECT * FROM dbsp_track('fresh')");
  db.exec("SELECT * FROM dbsp_sync('fresh')");
  db.exec(
      "SELECT * FROM dbsp_create_view('tf', 'SELECT SUM(v) AS s FROM fresh')");
  db.exec("INSERT INTO fresh VALUES (2, 4.0)");
  REQUIRE(view_sum(db, "tf") == sql_sum(db, "SELECT SUM(v) FROM fresh"));
  // The explicit dbsp_sync above is not decoration. On a connection that has
  // had ANY dbsp_track fail, a later dbsp_create_view does not seed its
  // baseline from the table — measured with `dbsp_track('no_such_table')`,
  // the oldest failure path there is, so this is not the precheck's doing.
  // Recorded rather than worked around silently.

  db.exec("DETACH old");
  std::remove(path.c_str());
}

TEST_CASE("trigger source: a racing sink CREATE does not fail the statement",
          "[trigger_source]") {
  // The sweep's DDL runs on an INTERNAL connection, in its own transaction,
  // concurrently with whatever the user's connections are doing — and
  // `CREATE TABLE IF NOT EXISTS` is not atomic against a concurrent creator of
  // the same entry. Measured in NumPad: building several materialized views in
  // a row on a worker thread, the sweep's sink CREATE lost that race and
  // DuckDB's `Catalog write-write conflict on create with "Schema main Table
  // dbsp_trigger_sink"` came out of the USER's statement.
  //
  // The race is made deterministic here by holding an uncommitted creator of
  // that exact entry open on a second connection while the sweep runs.
  DuckDBTestHarness db;
  db.createTable("t", "id INTEGER, v DOUBLE", {"(1, 5.0)"});

  duckdb::Connection blocker(db.instance());
  REQUIRE_FALSE(blocker.Query("BEGIN TRANSACTION")->HasError());
  REQUIRE_FALSE(
      blocker.Query("CREATE TABLE dbsp_trigger_sink (v BIGINT)")->HasError());

  db.exec("SELECT * FROM dbsp_track('t')");
  // This statement's QueryBegin is where the sweep tries to create the sink.
  // It must survive: a conflict is a DEFERRAL (recheck stays armed, the commit
  // reconciles by scan), never an error on the user's statement.
  auto res = db.query("SELECT 42");
  INFO("sweep under a racing creator: "
       << (res->HasError() ? res->GetError() : std::string("ok")));
  REQUIRE_FALSE(res->HasError());
  REQUIRE(res->GetValue(0, 0).GetValue<int64_t>() == 42);

  // Deferred, not abandoned: once the blocker lets go, the next statement
  // installs and the source works.
  REQUIRE_FALSE(blocker.Query("ROLLBACK")->HasError());
  db.exec("SELECT 1");
  REQUIRE(sql_count(db, "SELECT count(*) FROM duckdb_triggers()") == 3);
  db.exec("SELECT * FROM dbsp_sync('t')");
  db.exec("SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')");
  db.exec("INSERT INTO t VALUES (2, 7.0)");
  REQUIRE(view_sum(db, "tot") == sql_sum(db, "SELECT SUM(v) FROM t"));
}

TEST_CASE("cdc: create_view seeds its source baseline, not an empty one",
          "[trigger_source][create_view]") {
  // The public dbsp_track() creates a TrackedTable with an EMPTY baseline on
  // purpose ("Initial table sync deferred... call dbsp_sync() after
  // dbsp_track()"), and create_view's replay streams that baseline. It looked
  // correct only by accident: some unrelated commit normally ran a scan-sync
  // between the track and the create, and seeded it.
  //
  // Remove the accident — any earlier FAILED DBSP call shifts the commit
  // sequencing — and the view is built over NOTHING: a permanently wrong
  // answer, no error, no counter moving, and no self-healing. Measured before
  // the fix: view 0.0 where SQL read 3.0; after one insert view 3.0 / SQL 6.0;
  // after another view 7.0 / SQL 10.0 — the same constant offset forever.
  auto check_seeded = [](DuckDBTestHarness &db) {
    db.exec("SELECT * FROM dbsp_track('fresh')");
    db.exec("SELECT * FROM dbsp_create_view('tf', 'SELECT SUM(v) AS s FROM "
            "fresh')");
    REQUIRE(view_sum(db, "tf") == sql_sum(db, "SELECT SUM(v) FROM fresh"));
    // and it tracks edits from there, rather than carrying a constant offset
    db.exec("INSERT INTO fresh VALUES (2, 3.0)");
    REQUIRE(view_sum(db, "tf") == sql_sum(db, "SELECT SUM(v) FROM fresh"));
    db.exec("INSERT INTO fresh VALUES (3, 4.0)");
    REQUIRE(view_sum(db, "tf") == sql_sum(db, "SELECT SUM(v) FROM fresh"));
  };

  SECTION("happy path, unchanged") {
    DuckDBTestHarness db;
    db.createTable("fresh", "id INTEGER, v DOUBLE", {"(1, 3.0)"});
    check_seeded(db);
  }

  SECTION("after a failed dbsp_track") {
    DuckDBTestHarness db;
    REQUIRE(db.query("SELECT * FROM dbsp_track('no_such_table')")->HasError());
    db.createTable("fresh", "id INTEGER, v DOUBLE", {"(1, 3.0)"});
    check_seeded(db);
  }

  SECTION("after a failed dbsp_create_view") {
    DuckDBTestHarness db;
    REQUIRE(db.query("SELECT * FROM dbsp_create_view('bad', "
                     "'SELECT 1 FROM no_such_table')")
                ->HasError());
    db.createTable("fresh", "id INTEGER, v DOUBLE", {"(1, 3.0)"});
    check_seeded(db);
  }

  SECTION("failure AFTER the table exists") {
    DuckDBTestHarness db;
    db.createTable("fresh", "id INTEGER, v DOUBLE", {"(1, 3.0)"});
    REQUIRE(db.query("SELECT * FROM dbsp_track('no_such_table')")->HasError());
    check_seeded(db);
  }

  // The FILE-BACKED reproduction lives in
  // test/python/test_create_view_seeding.py: this harness is in-memory by
  // construction, and the reviewer found the defect on a file-backed database.
}

TEST_CASE("trigger source: create_view over a pre-v2.0.0 source is refused",
          "[trigger_source]") {
  // dbsp_track is not the only way into the tracked set: create_view
  // auto-tracks its sources. Before the precheck reached that route, a
  // CREATE MATERIALIZED VIEW over a v1.0.0 source SUCCEEDED and tracked the
  // table, and every statement afterwards — including COMMIT and ROLLBACK —
  // threw the sweep's error, with DETACH the only escape.
  DuckDBTestHarness db;
  const std::string path =
      std::string(std::tmpnam(nullptr)) + "_dbsp_v1v.duckdb";
  db.exec("ATTACH '" + path + "' AS old (STORAGE_VERSION 'v1.0.0')");
  db.exec("CREATE TABLE old.t (id INTEGER, v DOUBLE)");
  db.exec("INSERT INTO old.t VALUES (1, 5.0)");

  auto res = db.query("SELECT * FROM dbsp_create_view('vold', 'SELECT SUM(v) "
                      "AS s FROM old.t')");
  REQUIRE(res->HasError());
  const std::string err = res->GetError();
  INFO(err);
  REQUIRE(err.find("storage version") != std::string::npos);
  REQUIRE(err.find("STORAGE_VERSION 'v2.0.0'") != std::string::npos);

  // Nothing tracked, and the connection is still usable — including a
  // transaction that opens and closes cleanly, which is what the sweep's
  // throw used to break.
  REQUIRE(sql_count(db, "SELECT count(*) FROM dbsp_tables() "
                        "WHERE table_name LIKE '%.t'") == 0);
  REQUIRE(sql_count(db, "SELECT 1") == 1);
  db.exec("BEGIN TRANSACTION");
  REQUIRE(sql_count(db, "SELECT count(*) FROM old.t") == 1);
  db.exec("COMMIT");
  db.exec("BEGIN TRANSACTION");
  db.exec("ROLLBACK");

  db.exec("DETACH old");
  std::remove(path.c_str());
}
