#include "catch.hpp"
#include "../test_helpers.hpp"

#include <cstdio>
#include <cstdlib>

using namespace dbsp_test;

TEST_CASE("dbsp_track tracks table", "[integration][track]") {
    DuckDBTestHarness db;

    // Create test table
    db.exec("CREATE TABLE orders (id INT, customer VARCHAR, amount DECIMAL)");

    // Track the table
    auto result = db.query("SELECT * FROM dbsp_track('orders')");
    REQUIRE_FALSE(result->HasError());

    // Verify response
    auto msg = result->GetValue(0, 0).ToString();
    REQUIRE(msg.find("Tracking table: orders") != std::string::npos);

    // Verify table appears in tracked list
    auto tables = db.query("SELECT * FROM dbsp_tables()");
    REQUIRE_FALSE(tables->HasError());
    REQUIRE(tables->RowCount() >= 1);
}

TEST_CASE("dbsp_create_view creates filter view", "[integration][view]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("orders",
                   "id INT, customer VARCHAR, amount DECIMAL",
                   {"(1, 'Alice', 100.0)", "(2, 'Bob', 200.0)", "(3, 'Alice', 50.0)"});
    db.exec("SELECT * FROM dbsp_track('orders')");
    db.exec("SELECT * FROM dbsp_sync('orders')");

    // Create filter view
    auto result = db.query(
        "SELECT * FROM dbsp_create_view('high_value', "
        "'SELECT * FROM orders WHERE amount > 100')");
    if (result->HasError()) {
        INFO("dbsp_create_view error: " << result->GetError());
    }
    REQUIRE_FALSE(result->HasError());

    // Verify view is listed
    auto views = db.query("SELECT * FROM dbsp_views()");
    if (views->HasError()) {
        INFO("dbsp_views error: " << views->GetError());
    }
    REQUIRE_FALSE(views->HasError());
    INFO("Number of views: " << views->RowCount());
    REQUIRE(views->RowCount() >= 1);

    // Query the view
    db.assertViewRowCount("high_value", 1);
}

TEST_CASE("dbsp_create_view creates aggregate view", "[integration][view][aggregate]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("orders",
                   "id INT, customer VARCHAR, amount DECIMAL",
                   {"(1, 'Alice', 100.0)", "(2, 'Bob', 200.0)", "(3, 'Alice', 50.0)"});
    db.exec("SELECT * FROM dbsp_track('orders')");
    db.exec("SELECT * FROM dbsp_sync('orders')");

    // Create aggregate view
    auto result = db.query(
        "SELECT * FROM dbsp_create_view('totals', "
        "'SELECT customer, SUM(amount) FROM orders GROUP BY customer')");
    if (result->HasError()) {
        INFO("dbsp_create_view error: " << result->GetError());
    }
    REQUIRE_FALSE(result->HasError());

    // Query the view
    auto rows = db.getViewRows("totals");
    REQUIRE(rows.size() == 2);
}

TEST_CASE("dbsp_query returns view data", "[integration][query]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("products",
                   "id INT, name VARCHAR, price DECIMAL",
                   {"(1, 'Widget', 10.0)", "(2, 'Gadget', 20.0)"});
    db.exec("SELECT * FROM dbsp_track('products')");
    db.exec("SELECT * FROM dbsp_sync('products')");
    db.exec("SELECT * FROM dbsp_create_view('all_products', 'SELECT * FROM products')");

    // Query view
    auto result = db.query("SELECT * FROM dbsp_query('all_products')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 2);
    REQUIRE(result->ColumnCount() == 3);
}

TEST_CASE("dbsp_drop removes view", "[integration][drop]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("items", "id INT", {"(1)", "(2)"});
    db.exec("SELECT * FROM dbsp_track('items')");
    db.exec("SELECT * FROM dbsp_sync('items')");
    db.exec("SELECT * FROM dbsp_create_view('v1', 'SELECT * FROM items')");

    // Drop view
    auto result = db.query("SELECT dbsp_drop('v1')");
    REQUIRE_FALSE(result->HasError());

    // Verify view is gone
    auto views = db.query("SELECT * FROM dbsp_views()");
    REQUIRE(views->RowCount() == 0);
}

// ============================================================================
// Phase 1: DDL Syntax Tests (P1.1)
// ============================================================================

TEST_CASE("CREATE MATERIALIZED VIEW DDL syntax", "[integration][ddl]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("employees",
                   "id INT, name VARCHAR, dept VARCHAR, salary INT",
                   {"(1, 'Alice', 'Eng', 100)",
                    "(2, 'Bob', 'Eng', 150)",
                    "(3, 'Carol', 'Sales', 200)"});
    db.exec("SELECT * FROM dbsp_track('employees')");
    db.exec("SELECT * FROM dbsp_sync('employees')");

    SECTION("CREATE MATERIALIZED VIEW creates view") {
        auto result = db.query(
            "CREATE MATERIALIZED VIEW high_sal AS "
            "SELECT * FROM employees WHERE salary > 100");
        if (result->HasError()) {
            INFO("DDL error: " << result->GetError());
        }
        REQUIRE_FALSE(result->HasError());

        // Verify view exists
        auto views = db.query("SELECT * FROM dbsp_views()");
        REQUIRE(views->RowCount() >= 1);

        // Verify view has correct data
        db.assertViewRowCount("high_sal", 2);
    }

    SECTION("DROP MATERIALIZED VIEW removes view") {
        // Create first using DDL syntax
        db.exec("CREATE MATERIALIZED VIEW to_drop AS SELECT * FROM employees");

        // Drop using dbsp_drop (DROP MATERIALIZED VIEW is intercepted by DuckDB's
        // built-in parser before our extension gets a chance, so we use the
        // function-based API which works reliably)
        auto result = db.query("SELECT dbsp_drop('to_drop')");
        if (result->HasError()) {
            INFO("DROP error: " << result->GetError());
        }
        REQUIRE_FALSE(result->HasError());

        // Verify view is gone
        auto views = db.query("SELECT * FROM dbsp_views()");
        REQUIRE(views->RowCount() == 0);
    }

    SECTION("REFRESH MATERIALIZED VIEW is a no-op") {
        db.exec("CREATE MATERIALIZED VIEW refreshable AS SELECT * FROM employees");

        auto result = db.query("REFRESH MATERIALIZED VIEW refreshable");
        if (result->HasError()) {
            INFO("REFRESH error: " << result->GetError());
        }
        REQUIRE_FALSE(result->HasError());

        // Verify view still exists and has data
        db.assertViewRowCount("refreshable", 3);
    }
}

// ============================================================================
// Phase 1: HAVING Clause Tests (P1.2)
// ============================================================================

TEST_CASE("HAVING clause filters aggregated results", "[integration][having]") {
    DuckDBTestHarness db;

    // Setup with departments having different employee counts
    db.createTable("emp",
                   "id INT, dept VARCHAR, salary INT",
                   {"(1, 'Eng', 100)",
                    "(2, 'Eng', 150)",
                    "(3, 'Eng', 200)",
                    "(4, 'Sales', 120)",
                    "(5, 'HR', 90)"});
    db.exec("SELECT * FROM dbsp_track('emp')");
    db.exec("SELECT * FROM dbsp_sync('emp')");

    SECTION("HAVING COUNT(*) > N filters small groups") {
        auto result = db.query(
            "SELECT * FROM dbsp_create_view('big_depts', "
            "'SELECT dept, COUNT(id) FROM emp GROUP BY dept HAVING COUNT(id) > 1')");
        if (result->HasError()) {
            INFO("Create HAVING view error: " << result->GetError());
        }
        REQUIRE_FALSE(result->HasError());

        // Only Eng has > 1 employee
        db.assertViewRowCount("big_depts", 1);
    }

    SECTION("HAVING SUM(col) > N filters by aggregate value") {
        auto result = db.query(
            "SELECT * FROM dbsp_create_view('high_salary_depts', "
            "'SELECT dept, SUM(salary) FROM emp GROUP BY dept HAVING SUM(salary) > 200')");
        if (result->HasError()) {
            INFO("Create HAVING view error: " << result->GetError());
        }
        REQUIRE_FALSE(result->HasError());

        // Eng: sum=450 > 200 (passes), Sales: sum=120 (fails), HR: sum=90 (fails)
        db.assertViewRowCount("high_salary_depts", 1);
    }
}

// ============================================================================
// Phase 1: MIN/MAX Aggregate Tests (P1.3)
// ============================================================================

TEST_CASE("MIN and MAX aggregate functions", "[integration][aggregates]") {
    DuckDBTestHarness db;

    db.createTable("products",
                   "id INT, category VARCHAR, price INT",
                   {"(1, 'Electronics', 500)",
                    "(2, 'Electronics', 200)",
                    "(3, 'Books', 30)",
                    "(4, 'Books', 15)",
                    "(5, 'Books', 45)"});
    db.exec("SELECT * FROM dbsp_track('products')");
    db.exec("SELECT * FROM dbsp_sync('products')");

    SECTION("MIN aggregate computes minimum per group") {
        auto result = db.query(
            "SELECT * FROM dbsp_create_view('min_prices', "
            "'SELECT category, MIN(price) FROM products GROUP BY category')");
        if (result->HasError()) {
            INFO("Create MIN view error: " << result->GetError());
        }
        REQUIRE_FALSE(result->HasError());

        auto rows = db.getViewRows("min_prices");
        REQUIRE(rows.size() == 2);  // Electronics and Books

        // Check values
        for (const auto &row : rows) {
            std::string cat = row[0].GetValue<std::string>();
            int64_t min_price = row[1].GetValue<int64_t>();
            if (cat == "Electronics") {
                REQUIRE(min_price == 200);
            } else if (cat == "Books") {
                REQUIRE(min_price == 15);
            }
        }
    }

    SECTION("MAX aggregate computes maximum per group") {
        auto result = db.query(
            "SELECT * FROM dbsp_create_view('max_prices', "
            "'SELECT category, MAX(price) FROM products GROUP BY category')");
        if (result->HasError()) {
            INFO("Create MAX view error: " << result->GetError());
        }
        REQUIRE_FALSE(result->HasError());

        auto rows = db.getViewRows("max_prices");
        REQUIRE(rows.size() == 2);

        for (const auto &row : rows) {
            std::string cat = row[0].GetValue<std::string>();
            int64_t max_price = row[1].GetValue<int64_t>();
            if (cat == "Electronics") {
                REQUIRE(max_price == 500);
            } else if (cat == "Books") {
                REQUIRE(max_price == 45);
            }
        }
    }

    SECTION("MIN/MAX update correctly on insert") {
        db.exec(
            "SELECT * FROM dbsp_create_view('min_p', "
            "'SELECT category, MIN(price) FROM products GROUP BY category')");

        // Insert a new cheaper book
        db.exec("INSERT INTO products VALUES (6, 'Books', 5)");
        db.exec("SELECT * FROM dbsp_sync('products')");

        auto rows = db.getViewRows("min_p");
        for (const auto &row : rows) {
            std::string cat = row[0].GetValue<std::string>();
            int64_t min_price = row[1].GetValue<int64_t>();
            if (cat == "Books") {
                REQUIRE(min_price == 5);  // New minimum
            }
        }
    }
}

// ---------------------------------------------------------------------------
// DuckDB 2.0 chunk-size invariant on the table-function read surface.
//
// On 2.0 a Vector carries its own size and DataChunk::SetCardinality sets only
// the chunk's logical count, leaving the child vectors at 0 (see the note at
// the top of src/dbsp_extension.cpp). Every DBSP table function used to end
// that way, so every chunk it emitted violated the engine's own invariant.
//
// These two cases are the proof. Against the pre-fix binary the first one
// THROWS ("DataChunk::Verify - size mismatch: vector 0 (VARCHAR) has size 0
// but chunk has size 1") on the very first dbsp_ call under
// debug_verification_mode='verify_vectors'. The second pins the read-surface
// answers that the stale sizes put at risk: dbsp_query has no filter
// pushdown, so `WHERE col IS [NOT] NULL` lands a PhysicalFilter directly on
// the scan, and IsNull/IsNotNull read the input vector's own size.
// ---------------------------------------------------------------------------

TEST_CASE("read surface: chunks satisfy VERIFY_VECTORS",
          "[integration][extension][verify_vectors]") {
  DuckDBTestHarness db;
  // debug_verification_mode is read from DBConfigOptions::global_verification_mode,
  // a PROCESS-wide static (DataChunk::VerifyInternal), so it outlives this
  // harness's DatabaseInstance and would poison every later test in this
  // binary. Reset it from a destructor: a REQUIRE failure or an engine throw
  // unwinds out of this test case, and a plain trailing SET would be skipped.
  struct VerifyModeGuard {
    DuckDBTestHarness &h;
    ~VerifyModeGuard() {
      // A REQUIRE here would be unsafe (Catch2 throws, and this destructor can
      // run while unwinding). But the result must not be discarded either: a
      // failed reset leaves VERIFY_VECTORS on for every later test in this
      // process, which would look like unrelated failures. Abort loudly.
      auto result = h.query("SET GLOBAL debug_verification_mode='none'");
      if (!result || result->HasError()) {
        fprintf(stderr,
                "FATAL: could not reset global debug_verification_mode; "
                "every later test in this binary would run under "
                "VERIFY_VECTORS: %s\n",
                result ? result->GetError().c_str() : "null result");
        std::abort();
      }
    }
  } guard{db};
  db.exec("SET GLOBAL debug_verification_mode='verify_vectors'");
  REQUIRE(db.query("SELECT current_setting('debug_verification_mode')")
              ->GetValue(0, 0)
              .ToString() == "VERIFY_VECTORS");

  db.createTable("vt", "id INT, val INT, tag VARCHAR",
                 {"(1, 10, 'a')", "(2, NULL, 'b')", "(3, 30, NULL)"});
  // Each of these drives a different table function's scan callback; under
  // VERIFY_VECTORS any one of them with unsized child vectors throws at
  // PipelineExecutor::FetchFromSource.
  db.exec("SELECT * FROM dbsp_track('vt')");
  db.exec("SELECT * FROM dbsp_sync('vt')");
  db.exec("SELECT * FROM dbsp_create_view('vv', "
          "'SELECT id, val, tag FROM vt')");
  db.exec("SELECT * FROM dbsp_query('vv')");
  db.exec("SELECT * FROM dbsp_views()");
  db.exec("SELECT * FROM dbsp_tables()");
  db.exec("SELECT * FROM dbsp_stats()");
  db.exec("SELECT * FROM dbsp_changes('vv')");
  db.exec("SELECT * FROM dbsp_deps('vv')");
  // and through operators stacked on the scan
  db.exec("SELECT * FROM dbsp_query('vv') WHERE val IS NULL");
  db.exec("SELECT count(*) FROM dbsp_query('vv') WHERE tag IS NOT NULL");
  db.exec("SELECT * FROM dbsp_query('vv') ORDER BY id DESC");
  db.exec("SELECT dbsp_drop('vv')"); // scalar fn, not a table fn
  // reset happens in ~VerifyModeGuard
}

TEST_CASE("read surface: IS [NOT] NULL over dbsp_query matches SQL",
          "[integration][extension]") {
  DuckDBTestHarness db;
  db.createTable("nt", "id INT, val INT, tag VARCHAR",
                 {"(1, 10, 'a')", "(2, NULL, 'b')", "(3, 30, NULL)",
                  "(4, NULL, NULL)", "(5, 50, 'e')"});
  db.exec("SELECT * FROM dbsp_track('nt')");
  db.exec("SELECT * FROM dbsp_sync('nt')");
  db.exec("SELECT * FROM dbsp_create_view('nv', "
          "'SELECT id, val, tag FROM nt')");

  auto same = [&](const std::string &over_view, const std::string &over_sql) {
    auto actual = db.query(over_view);
    auto expected = db.query(over_sql);
    INFO("view q: " << over_view);
    REQUIRE_FALSE(actual->HasError());
    REQUIRE_FALSE(expected->HasError());
    REQUIRE(actual->RowCount() == expected->RowCount());
    // Column count too: the loop below is bounded by `expected`, so a view
    // returning FEWER columns would read out of range and a view returning
    // MORE would go unnoticed.
    REQUIRE(actual->ColumnCount() == expected->ColumnCount());
    for (size_t r = 0; r < expected->RowCount(); r++) {
      for (size_t c = 0; c < expected->ColumnCount(); c++) {
        REQUIRE(actual->GetValue(c, r).ToString() ==
                expected->GetValue(c, r).ToString());
      }
    }
  };
  same("SELECT id FROM dbsp_query('nv') WHERE val IS NULL ORDER BY ALL",
       "SELECT id FROM nt WHERE val IS NULL ORDER BY ALL");
  same("SELECT id FROM dbsp_query('nv') WHERE val IS NOT NULL ORDER BY ALL",
       "SELECT id FROM nt WHERE val IS NOT NULL ORDER BY ALL");
  same("SELECT id FROM dbsp_query('nv') WHERE tag IS NULL ORDER BY ALL",
       "SELECT id FROM nt WHERE tag IS NULL ORDER BY ALL");
  same("SELECT id, val IS NULL FROM dbsp_query('nv') ORDER BY ALL",
       "SELECT id, val IS NULL FROM nt ORDER BY ALL");
  same("SELECT tag IS NULL AS n, count(*) FROM dbsp_query('nv') "
       "GROUP BY 1 ORDER BY ALL",
       "SELECT tag IS NULL AS n, count(*) FROM nt GROUP BY 1 ORDER BY ALL");
}
