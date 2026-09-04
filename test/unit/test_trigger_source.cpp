// Differential oracle for the trigger-fed delta source
// (DBSP_DELTA_SOURCE=trigger, include/dbsp_trigger_source.hpp).
//
// This binary is built WITHOUT DBSP_ENGINE_HOOK, so nothing here can be served
// by the patched engine's commit callback — that is the point. Views must stay
// current fed only by the generated statement triggers, on a stock engine.
//
// The oracle is the engine-hook one (test/unit/test_engine_hook.cpp,
// test/integration/test_engine_hook_consumer.cpp): old images at weight -1,
// new at +1, update-then-delete of one row appears once in the old image and
// nowhere in the new, insert-then-delete in one transaction nets to zero,
// multi-table transactions apply in one pass, rollback discards everything.
// Every case also cross-checks dbsp_query against plain SQL, because a delta
// source that is self-consistently wrong would pass a weight assertion.
#include "../test_helpers.hpp"
#include "catch.hpp"
#include "dbsp_trigger_source.hpp"

#include "duckdb/main/appender.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace dbsp_test;

// Arm trigger mode before ANY harness runs. dbsp_native::delta_source() caches
// the environment on first call, which happens when the first harness loads the
// extension — well after this static initializer.
static const int g_trigger_mode_armed = [] {
  setenv("DBSP_DELTA_SOURCE", "trigger", 1);
  return 1;
}();

namespace {

// A sum view is the sharpest double-count detector available: a row ingested
// twice doubles the sum, where a row count would only show it if the view kept
// duplicates.
double view_sum(DuckDBTestHarness &db, const std::string &view) {
  auto rows = db.getViewRows(view);
  if (rows.empty()) {
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

TEST_CASE("trigger source: mode is armed and triggers get installed",
          "[trigger_source]") {
  REQUIRE(g_trigger_mode_armed == 1);
  REQUIRE(dbsp_native::trigger_source_enabled());

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
  // that disarms the capture stack flipped as a result.
  REQUIRE(dbsp_native::trigger_source_stats().trigger_syncs.load() >
          syncs_before);
  REQUIRE(dbsp_native::trigger_source_active());
}

TEST_CASE("trigger source: one insert counts exactly once (no double count)",
          "[trigger_source]") {
  // The capture stack and the plan tee must NOT also deliver these rows. A
  // second delivery would double the sum; the row count alone would not show
  // it, because the view would still hold one distinct row.
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
  // The whole reason the capture stack exists is write paths the binder never
  // sees. In 2.0 Appender::FlushInternal runs an INSERT ... SELECT, so the
  // binder — and therefore the trigger — does see it.
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
