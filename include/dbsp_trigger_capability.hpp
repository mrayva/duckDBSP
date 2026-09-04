#pragma once
// Can a given database carry DBSP's change-capture triggers?
//
// Its own header because BOTH sides of the tracked-table gate need it and they
// sit on opposite sides of an include edge: the trigger source
// (dbsp_trigger_source.hpp) checks it when it sweeps, and the CDC core
// (dbsp_cdc.hpp) checks it when a view auto-tracks a source — and the CDC
// core must not depend on the trigger source.

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/transaction/transaction_context.hpp"

#include <string>

namespace dbsp_native {

// catalog.schema.table -> the three parts. Keys are produced by
// canonical_table_key, so exactly two dots separate three non-empty parts.
inline bool split_table_key(const std::string &key, std::string &catalog,
                            std::string &schema, std::string &table) {
  const size_t d1 = key.find('.');
  if (d1 == std::string::npos) {
    return false;
  }
  const size_t d2 = key.find('.', d1 + 1);
  if (d2 == std::string::npos) {
    return false;
  }
  catalog = key.substr(0, d1);
  schema = key.substr(d1 + 1, d2 - d1 - 1);
  table = key.substr(d2 + 1);
  return !catalog.empty() && !schema.empty() && !table.empty();
}

/// Can this catalog hold the change-capture triggers at all?
///
/// `CREATE TRIGGER` needs storage version v2.0.0 or higher — the engine's own
/// test is duckdb/src/planner/binder/statement/bind_create.cpp:680-692, and
/// this mirrors it exactly (in-memory and temporary databases are exempt
/// there, so they are exempt here). On false, `version_out` carries the
/// version the file actually has, for the error message.
///
/// It is mirrored rather than left to the engine because of WHERE the engine's
/// error lands: the install runs from the sweep, i.e. from the QueryBegin of
/// some LATER statement, so `dbsp_track` on a v1.0.0 file used to SUCCEED and
/// every statement afterwards — reads included — threw
/// `Binder Error: CREATE TRIGGER is only supported for storage versions
/// v2.0.0 and higher` with the table left tracked and the connection wedged.
inline bool catalog_supports_triggers(duckdb::ClientContext &context,
                                      const std::string &catalog_name,
                                      std::string &version_out) {
  try {
    auto &catalog =
        duckdb::Catalog::GetCatalog(context, duckdb::Identifier(catalog_name));
    auto &attached = catalog.GetAttached();
    if (attached.IsTemporary() || !attached.HasStorageManager()) {
      return true;
    }
    auto &storage = attached.GetStorageManager();
    if (storage.InMemory() || !storage.HasStorageVersion()) {
      return true;
    }
    if (storage.GetStorageVersion() >= duckdb::StorageVersion::V2_0_0) {
      return true;
    }
    version_out =
        duckdb::GetStorageVersionName(storage.GetStorageVersion(), true);
    return false;
  } catch (...) {
    // Not a DuckDB catalog, detached mid-flight, or an API that moved: say
    // nothing and let the engine's own refusal speak.
    return true;
  }
}

/// Throw ONE readable error, naming the migration, if `key` lives in a
/// database too old to carry the triggers. Callers use this BEFORE tracking or
/// installing, so the refusal arrives at the statement that asked for it.
inline void require_trigger_capable_catalog(duckdb::ClientContext &context,
                                            const std::string &key) {
  std::string catalog, schema_name, table;
  if (!split_table_key(key, catalog, schema_name, table)) {
    return;
  }
  std::string version;
  if (catalog_supports_triggers(context, catalog, version)) {
    return;
  }
  throw duckdb::InvalidInputException(
      "DBSP cannot track '%s': its database is at storage version %s, and the "
      "change-capture triggers require v2.0.0 or higher. Rewrite the file "
      "first:\n"
      "  ATTACH '<old>.duckdb' AS src (READ_ONLY);\n"
      "  ATTACH '<new>.duckdb' AS dst (STORAGE_VERSION 'v2.0.0');\n"
      "  COPY FROM DATABASE src TO dst;\n"
      "then move the '<old>.duckdb.dbsp_spill' directory alongside the new "
      "file. The table has NOT been tracked.",
      key, version);
}

/// True while the USER holds an explicit transaction open. Autocommit
/// statements also have an active transaction by the time QueryBegin runs
/// (BeginQueryInternal starts it first), so the auto-commit flag is the half
/// that actually distinguishes them.
///
/// It matters because the reconcile's DDL runs on a separate internal
/// connection, which by construction CANNOT see the user's uncommitted
/// catalog changes. Running it anyway is what produced
/// `Binder Error: Referenced column "note" not found` out of the user's own
/// COMMIT, and `Catalog Error: Table with name u does not exist!` out of every
/// statement — including ROLLBACK — after a CREATE + track in one
/// transaction.
inline bool user_transaction_open(duckdb::ClientContext &context) {
  return context.transaction.HasActiveTransaction() &&
         !context.transaction.IsAutoCommit();
}

} // namespace dbsp_native
