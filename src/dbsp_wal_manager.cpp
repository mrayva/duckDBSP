#include "dbsp_wal_manager.hpp"
#include "dbsp_cdc.hpp"
#include <chrono>
#include <iostream>
#include <filesystem>
#include <limits>
#include <vector>

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

namespace dbsp_native {

// Global WAL manager instance
static std::unique_ptr<DBSPWALManager> g_wal_manager = nullptr;

DBSPWALManager& get_wal_manager() {
  if (!g_wal_manager) {
    g_wal_manager = std::make_unique<DBSPWALManager>();
  }
  return *g_wal_manager;
}

namespace {

constexpr uint32_t kMaxRowColumns = 10000;
constexpr uint32_t kMaxValueBytes = 64 * 1024 * 1024;

void write_value(std::ostream &out, const duckdb::Value &value) {
  duckdb::MemoryStream stream;
  duckdb::BinarySerializer serializer(stream);
  serializer.Begin();
  value.Serialize(serializer);
  serializer.End();

  const auto size = stream.GetPosition();
  const uint32_t encoded_size = duckdb::NumericCast<uint32_t>(size);
  out.write(reinterpret_cast<const char *>(&encoded_size), sizeof(encoded_size));
  out.write(reinterpret_cast<const char *>(stream.GetData()), encoded_size);
}

bool read_value(std::istream &in, duckdb::Value &value) {
  uint32_t encoded_size = 0;
  in.read(reinterpret_cast<char *>(&encoded_size), sizeof(encoded_size));
  if (!in.good() || encoded_size > kMaxValueBytes) {
    return false;
  }
  std::vector<uint8_t> bytes(encoded_size);
  in.read(reinterpret_cast<char *>(bytes.data()), encoded_size);
  if (!in.good()) {
    return false;
  }

  duckdb::MemoryStream stream(bytes.data(), bytes.size());
  duckdb::BinaryDeserializer deserializer(stream);
  deserializer.Begin();
  value = duckdb::Value::Deserialize(deserializer);
  deserializer.End();
  return true;
}

void write_row(std::ostream &out, const DuckDBRow &row) {
  const uint32_t column_count = duckdb::NumericCast<uint32_t>(row.columns.size());
  out.write(reinterpret_cast<const char *>(&column_count), sizeof(column_count));
  for (const auto &value : row.columns) {
    write_value(out, value);
  }
}

bool read_row(std::istream &in, DuckDBRow &row) {
  uint32_t column_count = 0;
  in.read(reinterpret_cast<char *>(&column_count), sizeof(column_count));
  if (!in.good() || column_count > kMaxRowColumns) {
    return false;
  }
  std::vector<duckdb::Value> columns;
  columns.reserve(column_count);
  for (uint32_t i = 0; i < column_count; i++) {
    duckdb::Value value;
    if (!read_value(in, value)) {
      return false;
    }
    columns.push_back(std::move(value));
  }
  row.columns.assign(std::move(columns));
  return true;
}

} // namespace

// WALEntry implementation. Values use DuckDB's native serializer so replay
// preserves types, NULLs, nested values, and decimals instead of converting
// every field through Value::ToString().
void WALEntry::write(std::ostream &out) const {
  // Write timestamp
  out.write(reinterpret_cast<const char *>(&timestamp), sizeof(timestamp));

  // Write operation type
  uint8_t op_type = static_cast<uint8_t>(operation_type);
  out.write(reinterpret_cast<const char *>(&op_type), sizeof(op_type));

  // Write table name (length-prefixed string)
  uint32_t name_len = static_cast<uint32_t>(table_name.size());
  out.write(reinterpret_cast<const char *>(&name_len), sizeof(name_len));
  out.write(table_name.data(), name_len);

  // Write operation-specific data
  switch (operation_type) {
    case WALOperationType::TABLE_INSERT:
    case WALOperationType::TABLE_DELETE: {
      write_row(out, row_data);
      break;
    }

    case WALOperationType::TABLE_UPDATE: {
      write_row(out, old_row_data);
      write_row(out, row_data);
      break;
    }

    case WALOperationType::VIEW_CREATE: {
      // Write view SQL
      uint32_t sql_len = static_cast<uint32_t>(view_sql.size());
      out.write(reinterpret_cast<const char *>(&sql_len), sizeof(sql_len));
      out.write(view_sql.data(), sql_len);
      break;
    }

    case WALOperationType::VIEW_DROP:
      // No additional data
      break;

    case WALOperationType::CHECKPOINT_MARKER: {
      // Write checkpoint timestamp
      out.write(reinterpret_cast<const char *>(&checkpoint_marker), sizeof(checkpoint_marker));
      break;
    }
  }
}

bool WALEntry::read(std::istream &in) {
  // Read timestamp
  in.read(reinterpret_cast<char *>(&timestamp), sizeof(timestamp));
  if (!in.good()) return false;

  // Read operation type
  uint8_t op_type;
  in.read(reinterpret_cast<char *>(&op_type), sizeof(op_type));
  if (!in.good()) return false;
  operation_type = static_cast<WALOperationType>(op_type);

  // Read table name
  uint32_t name_len;
  in.read(reinterpret_cast<char *>(&name_len), sizeof(name_len));
  if (!in.good() || name_len > 1024) return false;

  table_name.resize(name_len);
  in.read(&table_name[0], name_len);
  if (!in.good()) return false;

  // Read operation-specific data
  switch (operation_type) {
    case WALOperationType::TABLE_INSERT:
    case WALOperationType::TABLE_DELETE: {
      return read_row(in, row_data);
    }

    case WALOperationType::TABLE_UPDATE: {
      return read_row(in, old_row_data) && read_row(in, row_data);
    }

    case WALOperationType::VIEW_CREATE: {
      uint32_t sql_len;
      in.read(reinterpret_cast<char *>(&sql_len), sizeof(sql_len));
      if (!in.good() || sql_len > 1024 * 1024) return false;

      view_sql.resize(sql_len);
      in.read(&view_sql[0], sql_len);
      if (!in.good()) return false;
      break;
    }

    case WALOperationType::VIEW_DROP:
      // No additional data
      break;

    case WALOperationType::CHECKPOINT_MARKER: {
      in.read(reinterpret_cast<char *>(&checkpoint_marker), sizeof(checkpoint_marker));
      if (!in.good()) return false;
      break;
    }

    default:
      return false;  // Unknown operation type
  }

  return true;
}

// DBSPWALManager implementation
DBSPWALManager::DBSPWALManager(const std::string &wal_path)
  : wal_path_(wal_path.empty() ? ".dbsp_recovery/wal.dbsp" : wal_path),
    enabled_(true),
    last_checkpoint_pos_(0) {
}

DBSPWALManager::~DBSPWALManager() {
  if (wal_file_.is_open()) {
    flush();
    wal_file_.close();
  }
}

bool DBSPWALManager::initialize() {
  if (!enabled_) return true;

  std::lock_guard<std::mutex> lock(wal_mutex_);

  // Close any existing handle first to get accurate file existence check
  if (wal_file_.is_open()) {
    wal_file_.close();
  }

  // If file already exists, just open it
  if (std::filesystem::exists(wal_path_)) {
    wal_file_.open(wal_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    return wal_file_.is_open();
  }

  // Create parent directory if needed
  std::filesystem::path wal_file_path(wal_path_);
  if (wal_file_path.has_parent_path()) {
    std::filesystem::create_directories(wal_file_path.parent_path());
  }

  // Open WAL file in append mode
  wal_file_.open(wal_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);

  if (!wal_file_) {
    // File doesn't exist, create it
    wal_file_.open(wal_path_, std::ios::out | std::ios::binary);
    if (!wal_file_) {
      last_error_ = "Failed to create WAL file: " + wal_path_;
      return false;
    }
    wal_file_.close();

    // Reopen in read/write mode
    wal_file_.open(wal_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    if (!wal_file_) {
      last_error_ = "Failed to reopen WAL file: " + wal_path_;
      return false;
    }
  }

  return true;
}

uint64_t DBSPWALManager::get_current_timestamp() const {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
}

bool DBSPWALManager::write_entry(const WALEntry &entry) {
  if (!enabled_ || !wal_file_.is_open()) return false;

  std::lock_guard<std::mutex> lock(wal_mutex_);

  try {
    entry.write(wal_file_);
    return wal_file_.good();
  } catch (const std::exception &e) {
    last_error_ = std::string("Failed to write WAL entry: ") + e.what();
    return false;
  }
}

bool DBSPWALManager::log_insert(const std::string &table_name, const DuckDBRow &row) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::TABLE_INSERT;
  entry.table_name = table_name;
  entry.row_data = row;

  return write_entry(entry);
}

bool DBSPWALManager::log_delete(const std::string &table_name, const DuckDBRow &row) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::TABLE_DELETE;
  entry.table_name = table_name;
  entry.row_data = row;

  return write_entry(entry);
}

bool DBSPWALManager::log_update(const std::string &table_name,
                                const DuckDBRow &old_row,
                                const DuckDBRow &new_row) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::TABLE_UPDATE;
  entry.table_name = table_name;
  entry.old_row_data = old_row;
  entry.row_data = new_row;

  return write_entry(entry);
}

bool DBSPWALManager::log_view_create(const std::string &view_name, const std::string &sql) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::VIEW_CREATE;
  entry.table_name = view_name;
  entry.view_sql = sql;

  return write_entry(entry);
}

bool DBSPWALManager::log_view_drop(const std::string &view_name) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::VIEW_DROP;
  entry.table_name = view_name;

  return write_entry(entry);
}

bool DBSPWALManager::log_checkpoint_marker(uint64_t checkpoint_timestamp) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::CHECKPOINT_MARKER;
  entry.checkpoint_marker = checkpoint_timestamp;

  bool result = write_entry(entry);
  if (result) {
    last_checkpoint_pos_ = wal_file_.tellp();
  }

  return result;
}

bool DBSPWALManager::flush() {
  if (!enabled_ || !wal_file_.is_open()) return true;

  std::lock_guard<std::mutex> lock(wal_mutex_);

  wal_file_.flush();
  return wal_file_.good();
}

bool DBSPWALManager::replay_wal(duckdb::ClientContext &context, uint64_t after_timestamp) {
  if (!enabled_) return true;

  std::lock_guard<std::mutex> lock(wal_mutex_);

  if (!wal_file_.is_open()) {
    last_error_ = "WAL file not open";
    return false;
  }

  // Save current position
  auto current_pos = wal_file_.tellg();

  // Seek to beginning
  wal_file_.seekg(0, std::ios::beg);

  auto &cdc_manager = get_cdc_manager(context);
  size_t entries_replayed = 0;

  if (std::getenv("DBSP_DEBUG_WAL")) {
    std::cerr << "[dbsp] WAL replay after timestamp " << after_timestamp
              << "\n";
  }

  try {
    while (wal_file_.good() && !wal_file_.eof()) {
      WALEntry entry;
      if (!entry.read(wal_file_)) {
        if (wal_file_.eof()) break;
        last_error_ = "Failed to read WAL entry";
        wal_file_.seekg(current_pos);
        return false;
      }

      // Skip entries before threshold
      if (entry.timestamp <= after_timestamp) {
        continue;
      }

      // Replay entry
      switch (entry.operation_type) {
        case WALOperationType::TABLE_INSERT:
          cdc_manager.on_insert(entry.table_name, entry.row_data, &context);
          entries_replayed++;
          break;

        case WALOperationType::TABLE_DELETE:
          cdc_manager.on_delete(entry.table_name, entry.row_data, &context);
          entries_replayed++;
          break;

        case WALOperationType::TABLE_UPDATE:
          cdc_manager.on_update(entry.table_name, entry.old_row_data, entry.row_data,
                                &context);
          entries_replayed++;
          break;

        case WALOperationType::VIEW_CREATE:
          cdc_manager.create_view(context, entry.table_name, entry.view_sql);
          entries_replayed++;
          break;

        case WALOperationType::VIEW_DROP:
          cdc_manager.drop_view(entry.table_name, &context);
          entries_replayed++;
          break;

        case WALOperationType::CHECKPOINT_MARKER:
          // Update threshold for future truncation
          after_timestamp = entry.checkpoint_marker;
          break;
      }
    }

    if (std::getenv("DBSP_DEBUG_WAL")) {
      std::cerr << "[dbsp] WAL replayed " << entries_replayed << " entries\n";
    }

    // Restore position
    wal_file_.clear();  // Clear EOF flag
    wal_file_.seekg(current_pos);

    return true;

  } catch (const std::exception &e) {
    last_error_ = std::string("Exception during WAL replay: ") + e.what();
    wal_file_.seekg(current_pos);
    return false;
  }
}

bool DBSPWALManager::truncate_to_checkpoint(uint64_t checkpoint_timestamp) {
  if (!enabled_ || !wal_file_.is_open()) return true;

  std::lock_guard<std::mutex> lock(wal_mutex_);

  // Create new temp WAL file
  std::string temp_wal_path = wal_path_ + ".tmp";
  std::ofstream new_wal(temp_wal_path, std::ios::binary);

  if (!new_wal) {
    last_error_ = "Failed to create temp WAL file";
    return false;
  }

  // Seek to beginning
  wal_file_.seekg(0, std::ios::beg);

  // Copy entries after checkpoint timestamp
  try {
    while (wal_file_.good() && !wal_file_.eof()) {
      WALEntry entry;
      if (!entry.read(wal_file_)) {
        if (wal_file_.eof()) break;
        last_error_ = "Failed to read WAL entry during truncation";
        return false;
      }

      // Keep entries after checkpoint
      if (entry.timestamp > checkpoint_timestamp) {
        entry.write(new_wal);
      }
    }

    new_wal.close();
    wal_file_.close();

    // Replace old WAL with new one
    std::filesystem::remove(wal_path_);
    std::filesystem::rename(temp_wal_path, wal_path_);

    // Reopen WAL
    wal_file_.open(wal_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);

    if (std::getenv("DBSP_DEBUG_WAL")) {
      std::cerr << "[dbsp] WAL truncated before timestamp "
                << checkpoint_timestamp << "\n";
    }

    return wal_file_.is_open();

  } catch (const std::exception &e) {
    last_error_ = std::string("Exception during WAL truncation: ") + e.what();
    return false;
  }
}

} // namespace dbsp_native
