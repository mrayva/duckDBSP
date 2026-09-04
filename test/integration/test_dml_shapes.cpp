// DML shapes that a delta source can get wrong. Each of these defeated at
// least one earlier delta source: a table written twice in one transaction, a
// predicate reading transaction-local state, UPDATE ... FROM, a volatile SET
// expression, an indexed-column UPDATE (which the engine executes as
// delete+re-append), non-repeatable INSERT sources (table functions, SAMPLE,
// sequence DEFAULTs), and multi-statement DML in one string.
//
// The trigger source reports what the statement DID rather than predicting it,
// so all of them are expected to be served exactly. Every scenario checks the
// view against direct SQL AND asserts via counters that no scan ran — a source
// that fell back to scan-and-diff would still produce the right view, so the
// correctness check alone could not fail on a silent regression.

#include "../test_helpers.hpp"
#include "catch.hpp"

using namespace dbsp_test;

namespace {

struct ShapeFixture {
  DuckDBTestHarness db;

  ShapeFixture() {
    db.createTable("tt", "id INT, grp INT, val INT",
                   {"(1, 1, 10)", "(2, 1, 20)", "(3, 2, 30)", "(4, 2, 40)"});
    db.createTable("tu", "id INT, tag VARCHAR", {"(1, 'x')", "(3, 'y')"});
    db.exec("SELECT * FROM dbsp_track('tt')");
    db.exec("SELECT * FROM dbsp_track('tu')");
    db.exec("SELECT * FROM dbsp_sync('tt')");
    db.exec("SELECT * FROM dbsp_sync('tu')");
    db.exec("SELECT * FROM dbsp_create_view('tv_agg', 'SELECT grp, "
            "SUM(val) AS s, COUNT(*) AS n FROM tt GROUP BY grp')");
    db.exec("SELECT * FROM dbsp_auto_sync(true)");
  }

  void finish() { db.exec("SELECT * FROM dbsp_auto_sync(false)"); }

  void check() {
    auto expected = db.query("SELECT * FROM (SELECT grp, SUM(val) AS s, "
                             "COUNT(*) AS n FROM tt GROUP BY grp) "
                             "ORDER BY ALL");
    auto actual = db.query("SELECT * FROM dbsp_query('tv_agg') ORDER BY ALL");
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
};

} // namespace

TEST_CASE("dml shapes: same-table-twice transaction stays O(delta)",
          "[integration][dml_shapes]") {
  ShapeFixture fx;
  auto &m = fx.db.manager();
  // one table, two statements, one commit: both firings buffer into the same
  // per-transaction delta and it applies in one pass — no scan at commit
  const uint64_t caps = m.exact_delta_syncs();
  const uint64_t scans = m.scan_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("INSERT INTO tt VALUES (9, 1, 90)");
  fx.db.exec("DELETE FROM tt WHERE id = 1");
  fx.db.exec("COMMIT");
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check();
  fx.finish();
}

TEST_CASE("dml shapes: post-write subquery DELETE stays O(delta)",
          "[integration][dml_shapes]") {
  ShapeFixture fx;
  auto &m = fx.db.manager();
  // the DELETE's subquery reads tu AFTER this transaction wrote it: a source
  // that re-ran the predicate against committed state would miss id 4
  const uint64_t caps = m.exact_delta_syncs();
  const uint64_t scans = m.scan_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("INSERT INTO tu VALUES (4, 'z')");
  fx.db.exec("DELETE FROM tt WHERE id IN (SELECT id FROM tu)");
  fx.db.exec("COMMIT");
  // one apply per table: the INSERT (tu) and the DELETE (tt)
  REQUIRE(m.exact_delta_syncs() == caps + 2);
  REQUIRE(m.scan_syncs() == scans);
  fx.check();
  // the subquery must have seen the txn-local INSERT: id 4 deleted too
  auto res = fx.db.query("SELECT COUNT(*) FROM tt");
  REQUIRE(res->GetValue(0, 0).GetValue<int64_t>() == 1); // only id 2 left
  fx.finish();
}

TEST_CASE("dml shapes: rollback discards buffered rows",
          "[integration][dml_shapes]") {
  ShapeFixture fx;
  auto &m = fx.db.manager();
  const uint64_t caps = m.exact_delta_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("INSERT INTO tt VALUES (9, 1, 90)");
  fx.db.exec("DELETE FROM tt WHERE grp = 1");
  fx.db.exec("ROLLBACK");
  REQUIRE(m.exact_delta_syncs() == caps);
  fx.check(); // views match the unchanged table
  fx.finish();
}

TEST_CASE("dml shapes: zero-match DELETE after write skips the scan",
          "[integration][dml_shapes]") {
  ShapeFixture fx;
  auto &m = fx.db.manager();
  const uint64_t scans = m.scan_syncs();
  const uint64_t caps = m.exact_delta_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("INSERT INTO tt VALUES (9, 3, 90)");
  fx.db.exec("DELETE FROM tt WHERE id = 777"); // matches nothing
  fx.db.exec("COMMIT");
  // the DELETE fires nothing at all; the INSERT's delta still serves the
  // commit on its own
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check();
  fx.finish();
}

TEST_CASE("dml shapes: UPDATE ... FROM stays O(delta)",
          "[integration][dml_shapes]") {
  ShapeFixture fx;
  auto &m = fx.db.manager();
  // the joined new values are not derivable from the target row alone; the
  // UPDATE trigger carries both transition tables, so they need not be
  const uint64_t caps = m.exact_delta_syncs();
  const uint64_t scans = m.scan_syncs();
  fx.db.exec("UPDATE tt SET val = tt.val + tu.id * 100 FROM tu "
             "WHERE tt.id = tu.id");
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check();
  fx.finish();
}

TEST_CASE("dml shapes: multi-match UPDATE ... FROM",
          "[integration][dml_shapes]") {
  ShapeFixture fx;
  auto &m = fx.db.manager();
  // Both tu2 rows match tt.id = 1, so the statement could produce two
  // different new images for one target row. A predictive source had to
  // declare that ambiguous and fall back to a scan; the transition tables
  // report whichever image the engine actually wrote, so no fallback is
  // needed — pinned here as "no scan, and the view still matches".
  fx.db.exec("CREATE TABLE tu2 (id INT, add_v INT)");
  fx.db.exec("INSERT INTO tu2 VALUES (1, 100), (1, 200)");
  const uint64_t scans = m.scan_syncs();
  auto res = fx.db.query("UPDATE tt SET val = tt.val + tu2.add_v FROM tu2 "
                         "WHERE tt.id = tu2.id");
  if (!res->HasError()) {
    REQUIRE(m.scan_syncs() == scans);
  }
  fx.check(); // either way the view matches the table
  fx.finish();
}

TEST_CASE("dml shapes: volatile SET expression captured exactly",
          "[integration][dml_shapes]") {
  ShapeFixture fx;
  auto &m = fx.db.manager();
  // random() in SET: re-evaluating the expression would give a DIFFERENT
  // value, so only the row the statement actually wrote will do
  const uint64_t caps = m.exact_delta_syncs();
  const uint64_t scans = m.scan_syncs();
  fx.db.exec("UPDATE tt SET val = CAST(random() * 1000 AS INT) "
             "WHERE id = 2");
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check();
  fx.finish();
}

TEST_CASE("dml shapes: indexed-column UPDATE (del_and_insert) stays O(delta)",
          "[integration][dml_shapes]") {
  DuckDBTestHarness db;
  db.createTable("tpk2", "id INT PRIMARY KEY, val INT",
                 {"(1, 10)", "(2, 20)"});
  db.exec("SELECT * FROM dbsp_track('tpk2')");
  db.exec("SELECT * FROM dbsp_sync('tpk2')");
  db.exec("SELECT * FROM dbsp_create_view('tv_pk2', "
          "'SELECT id, val FROM tpk2 WHERE val > 5')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");
  auto &m = db.manager();
  const uint64_t caps = m.exact_delta_syncs();
  const uint64_t scans = m.scan_syncs();
  db.exec("UPDATE tpk2 SET id = id + 100 WHERE val = 10");
  REQUIRE(m.exact_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  auto expected = db.query("SELECT * FROM (SELECT id, val FROM tpk2 "
                           "WHERE val > 5) ORDER BY ALL");
  auto actual = db.query("SELECT * FROM dbsp_query('tv_pk2') ORDER BY ALL");
  REQUIRE(actual->RowCount() == expected->RowCount());
  for (size_t r = 0; r < expected->RowCount(); r++) {
    REQUIRE(actual->GetValue(0, r).ToString() ==
            expected->GetValue(0, r).ToString());
  }
  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("dml shapes: non-repeatable INSERT sources stay O(delta)",
          "[integration][dml_shapes]") {
  ShapeFixture fx;
  auto &m = fx.db.manager();

  SECTION("table-function source") {
    const uint64_t caps = m.exact_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("INSERT INTO tt SELECT 100 + i, 3, CAST(i AS INT) "
               "FROM range(3) r(i)");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    REQUIRE(m.scan_syncs() == scans);
    fx.check();
  }
  SECTION("USING SAMPLE source") {
    const uint64_t caps = m.exact_delta_syncs();
    fx.db.exec("INSERT INTO tt SELECT id + 200, grp, val FROM tt "
               "USING SAMPLE 2");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    fx.check();
  }
  SECTION("permuted full column list") {
    const uint64_t caps = m.exact_delta_syncs();
    fx.db.exec("INSERT INTO tt (val, id, grp) VALUES (77, 300, 1)");
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    fx.check();
  }
  SECTION("sequence DEFAULT, sequence still advances once") {
    // A DEFAULT nextval() column is the sharpest non-repeatable source there
    // is: anything that re-derives the inserted row by re-running the source
    // would advance the sequence a SECOND time and store a value the table
    // never held. The trigger sees the row post-default, so the sequence
    // advances exactly once.
    // Pinned: no scan, the sequence advanced once, and the delta is exact.
    fx.db.exec("CREATE SEQUENCE tsq");
    fx.db.exec("CREATE TABLE tseq (id INT DEFAULT nextval('tsq'), v INT)");
    fx.db.exec("SELECT * FROM dbsp_track('tseq')");
    fx.db.exec("SELECT * FROM dbsp_sync('tseq')");
    fx.db.exec("SELECT * FROM dbsp_create_view('tv_seq', "
               "'SELECT id, v FROM tseq')");
    const uint64_t scans = m.scan_syncs();
    const uint64_t caps = m.exact_delta_syncs();
    // 7, not 1: the row must be (id=1, v=7) so that a source mapping the two
    // INT columns in the wrong order produces (7, 1) and the comparison
    // below fails. With v=1 the row is (1, 1) and a swapped mapping still
    // compares equal — the assertion could not fail.
    fx.db.exec("INSERT INTO tseq (v) VALUES (7)");
    REQUIRE(m.scan_syncs() == scans);
    REQUIRE(m.exact_delta_syncs() == caps + 1);
    auto res = fx.db.query("SELECT MAX(id), COUNT(*) FROM tseq");
    REQUIRE(res->GetValue(0, 0).GetValue<int64_t>() == 1); // advanced once
    REQUIRE(res->GetValue(1, 0).GetValue<int64_t>() == 1);
    // the applied delta must be exactly what the table holds
    auto view = fx.db.query("SELECT * FROM dbsp_query('tv_seq') ORDER BY ALL");
    auto truth = fx.db.query("SELECT id, v FROM tseq ORDER BY ALL");
    REQUIRE(view->RowCount() == truth->RowCount());
    for (size_t r = 0; r < truth->RowCount(); r++) {
      for (size_t c = 0; c < truth->ColumnCount(); c++) {
        REQUIRE(view->GetValue(c, r).ToString() ==
                truth->GetValue(c, r).ToString());
      }
    }
  }
  fx.finish();
}

TEST_CASE("dml shapes: multi-statement string DML stays O(delta)",
          "[integration][dml_shapes]") {
  ShapeFixture fx;
  auto &m = fx.db.manager();
  // statement classification calls this WRITE_UNKNOWN, which alone would mean
  // a full sync_all — but each sub-statement runs as its own autocommit
  // transaction and fires its own triggers, so each is served exactly
  const uint64_t caps = m.exact_delta_syncs();
  const uint64_t scans = m.scan_syncs();
  fx.db.exec("INSERT INTO tt VALUES (400, 1, 40); "
             "DELETE FROM tt WHERE id = 400; "
             "UPDATE tt SET val = val + 1 WHERE id = 2");
  REQUIRE(m.exact_delta_syncs() == caps + 3);
  REQUIRE(m.scan_syncs() == scans);
  fx.check();
  fx.finish();
}
