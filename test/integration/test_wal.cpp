#include "catch.hpp"
#include "duckdb.hpp"
#include "dbsp_cdc.hpp"
#include "dbsp_wal_manager.hpp"

#include <filesystem>
#include <sstream>

using namespace duckdb;
using namespace dbsp_native;

TEST_CASE("WAL preserves typed rows and replays deltas", "[crash_recovery][wal]") {
  std::filesystem::remove_all(".dbsp_recovery");

  DuckDBRow original;
  original.columns.assign(std::vector<Value>{
      Value::INTEGER(7), Value::DOUBLE(3.5), Value(LogicalType::INTEGER)});

  WALEntry written;
  written.timestamp = 1;
  written.operation_type = WALOperationType::TABLE_INSERT;
  written.table_name = "events";
  written.row_data = original;

  std::stringstream encoded;
  written.write(encoded);

  WALEntry decoded;
  REQUIRE(decoded.read(encoded));
  REQUIRE(decoded.table_name == "events");
  REQUIRE(decoded.row_data.columns.size() == 3);
  REQUIRE(decoded.row_data.columns[0].type() == LogicalType::INTEGER);
  REQUIRE(decoded.row_data.columns[0].GetValue<int32_t>() == 7);
  REQUIRE(decoded.row_data.columns[1].type() == LogicalType::DOUBLE);
  REQUIRE(decoded.row_data.columns[1].GetValue<double>() == 3.5);
  REQUIRE(decoded.row_data.columns[2].type() == LogicalType::INTEGER);
  REQUIRE(decoded.row_data.columns[2].IsNull());

  DuckDB db;
  Connection con(db);
  con.Query("CREATE TABLE events (id INTEGER, amount DOUBLE, marker INTEGER)");

  auto &manager = get_cdc_manager(*con.context);
  INFO(manager.last_error());
  con.BeginTransaction();
  REQUIRE(manager.track_table(*con.context, "events"));
  con.Commit();
  const auto tracked_name = manager.list_tracked_tables().front();

  auto &wal = get_wal_manager();
  REQUIRE(wal.initialize());
  REQUIRE(wal.log_insert(tracked_name, original));
  REQUIRE(wal.flush());

  manager.on_insert(tracked_name, original, con.context.get());
  REQUIRE(manager.get_tracked_table_count(tracked_name) == 1);

  manager.clear_all_state();
  con.BeginTransaction();
  REQUIRE(manager.track_table(*con.context, "events"));
  con.Commit();
  REQUIRE(wal.replay_wal(*con.context, 0));
  REQUIRE(manager.get_tracked_table_count(tracked_name) == 1);

  const auto path = wal.get_wal_path();
  const auto before = std::filesystem::file_size(path);
  const auto marker = wal.get_current_timestamp();
  REQUIRE(wal.log_checkpoint_marker(marker));
  REQUIRE(wal.flush());
  REQUIRE(wal.log_insert(tracked_name, original));
  REQUIRE(wal.flush());
  REQUIRE(wal.truncate_to_checkpoint(marker));
  REQUIRE(std::filesystem::file_size(path) < before + 128);

  std::filesystem::remove_all(".dbsp_recovery");
}
