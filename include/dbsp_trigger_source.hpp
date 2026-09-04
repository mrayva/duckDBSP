#pragma once
// Trigger-fed delta source — the ONLY way this extension learns what a
// committing transaction wrote.
//
// DuckDB 2.0 has statement-level AFTER triggers with transition tables.
// `dbsp_track(t)` generates three of them on `t`
//
//   CREATE OR REPLACE TRIGGER dbsp_trg_<t>_ins AFTER INSERT ON <t>
//     REFERENCING NEW TABLE AS dbsp_new FOR EACH STATEMENT
//     INSERT INTO <sink> SELECT max(dbsp_trigger_ingest('<key>', 1, <cols>))
//                        FROM dbsp_new;
//
// (plus the DELETE/-1 and the UPDATE one, which carries BOTH images through a
// UNION ALL). The body is SQL — that is all the engine offers, there is no C++
// trigger API — so the row images reach C++ through a vectorised, volatile
// extension scalar that writes them into the committing connection's
// per-transaction buffer (DBSPContextState::buffer_trigger_delta), tagged with
// the canonical catalog.schema.table key and weight (-1 old image, +1 new
// image). TransactionCommit then applies them through the one existing ingest
// path, so the CDC core (dbsp_cdc.hpp) is untouched, and TransactionRollback
// discards them for free.
//
// Why it matters: trigger expansion is binder-level, and in 2.0 even the
// Appender goes through the binder (Appender::FlushInternal runs an
// INSERT ... SELECT). So this source needs NO patched engine — it is exact
// deltas on a STOCK DuckDB. What it costs is the write forms the engine
// refuses on a table carrying a trigger: MERGE INTO, ON CONFLICT DO UPDATE /
// INSERT OR REPLACE, and every ALTER TABLE except ADD COLUMN.
//
// There is no mode switch. Triggers are the only delta source.
//
// See docs/DESIGN_TRIGGER_SOURCE.md.

#include "dbsp_cdc.hpp"
#include "dbsp_qualified_name.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/transaction/transaction_context.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace dbsp_native {

// PROOF OF LIFE: this flag flips only when a trigger body has actually
// DELIVERED rows, never at install time. Until then the commit path keeps its
// pessimistic net — a transaction it cannot account for is reconciled by scan
// rather than assumed empty. Triggers that exist but never fire therefore cost
// scans, not silent staleness.
inline std::atomic<bool> &trigger_source_flag() {
  static std::atomic<bool> flag{false};
  return flag;
}
inline bool trigger_source_active() {
  return trigger_source_flag().load(std::memory_order_relaxed);
}

struct TriggerSourceStats {
  std::atomic<uint64_t> trigger_syncs{0}; // trigger-body ingest calls served
  std::atomic<uint64_t> trigger_rows{0};  // row images handed to the buffer
};

inline TriggerSourceStats &trigger_source_stats() {
  static TriggerSourceStats stats;
  return stats;
}

// Cheap process-wide gate for the sink drain, so the common commit path never
// touches the per-database map (see maybe_drain_trigger_sinks). Counts
// COMMITS, not trigger firings: the bodies write one sink row per statement
// whether or not this process has installed anything, so tying the drain to
// what this process installed left that growth unreclaimed.
inline std::atomic<uint64_t> &trigger_commits_total() {
  static std::atomic<uint64_t> n{0};
  return n;
}

// How many commits between sink drains. Overridable so the bound is testable
// without running fifty thousand statements.
inline uint64_t trigger_sink_drain_interval() {
  static const uint64_t n = [] {
    if (const char *v = std::getenv("DBSP_TRIGGER_SINK_DRAIN")) {
      const long long parsed = atoll(v);
      if (parsed > 0) {
        return static_cast<uint64_t>(parsed);
      }
    }
    return static_cast<uint64_t>(50000);
  }();
  return n;
}

// ---------------------------------------------------------------------------
// Row images: chunk columns [first_col, end) -> signed Z-set
// ---------------------------------------------------------------------------

// Vectorised, on the stream_table_rows idiom (dbsp_cdc.hpp): read typed
// vector data directly for the common types instead of boxing every cell, and
// pre-seed each row's hash from the vectorised chunk_row_hashes over EXACTLY
// the row columns. Hashing the whole args chunk instead would fold the key and
// weight arguments into the hash, and an old image at -1 would then never
// cancel the identical new image at +1.
inline void trigger_chunk_to_zset(duckdb::DataChunk &args,
                                  duckdb::idx_t first_col, int64_t weight,
                                  DuckDBZSet &out) {
  const duckdb::idx_t n = args.size();
  const duckdb::idx_t ncols = args.ColumnCount();
  if (n == 0 || first_col >= ncols) {
    return;
  }
  const duckdb::idx_t width = ncols - first_col;
  std::vector<std::vector<duckdb::Value>> vals(n);
  for (auto &r : vals) {
    r.reserve(width);
  }
  std::vector<duckdb::idx_t> cols;
  cols.reserve(width);
  for (duckdb::idx_t c = first_col; c < ncols; c++) {
    cols.push_back(c);
    auto &vec = args.data[c];
    vec.Flatten();
    const auto &type = vec.GetType();
    auto &validity = duckdb::FlatVector::Validity(vec);
    switch (type.id()) {
    case duckdb::LogicalTypeId::INTEGER: {
      auto data = duckdb::FlatVector::GetData<int32_t>(vec);
      for (duckdb::idx_t i = 0; i < n; i++) {
        vals[i].push_back(validity.RowIsValid(i) ? duckdb::Value::INTEGER(data[i])
                                                 : duckdb::Value(type));
      }
      break;
    }
    case duckdb::LogicalTypeId::BIGINT: {
      auto data = duckdb::FlatVector::GetData<int64_t>(vec);
      for (duckdb::idx_t i = 0; i < n; i++) {
        vals[i].push_back(validity.RowIsValid(i) ? duckdb::Value::BIGINT(data[i])
                                                 : duckdb::Value(type));
      }
      break;
    }
    case duckdb::LogicalTypeId::DOUBLE: {
      auto data = duckdb::FlatVector::GetData<double>(vec);
      for (duckdb::idx_t i = 0; i < n; i++) {
        vals[i].push_back(validity.RowIsValid(i) ? duckdb::Value::DOUBLE(data[i])
                                                 : duckdb::Value(type));
      }
      break;
    }
    case duckdb::LogicalTypeId::VARCHAR: {
      auto data = duckdb::FlatVector::GetData<duckdb::string_t>(vec);
      for (duckdb::idx_t i = 0; i < n; i++) {
        vals[i].push_back(validity.RowIsValid(i)
                              ? duckdb::Value(data[i].GetString())
                              : duckdb::Value(type));
      }
      break;
    }
    default:
      for (duckdb::idx_t i = 0; i < n; i++) {
        vals[i].push_back(vec.GetValue(i));
      }
      break;
    }
  }
  std::vector<size_t> row_hashes;
  chunk_row_hashes(args, cols, row_hashes);
  for (duckdb::idx_t i = 0; i < n; i++) {
    DuckDBRow row;
    row.columns.assign(std::move(vals[i]));
    row.columns.set_hash(row_hashes[i]);
    out.insert(std::move(row), weight);
  }
}

// ---------------------------------------------------------------------------
// Per-database install bookkeeping
// ---------------------------------------------------------------------------

struct TriggerInstallState {
  // Which database this state was built for. DuckDB REUSES freed
  // DatabaseInstance addresses, so a state found at a given address can be the
  // leftovers of a database that has since closed — and believing its
  // "already installed" list would leave a brand-new database's tables
  // triggerless and silently stale. Measured: it did exactly that across
  // consecutive test harnesses in one process, and a close-time prune alone
  // did NOT fix it (the teardown hook can return early before the prune).
  // Holding a weak reference makes the check exact instead of hopeful.
  duckdb::weak_ptr<duckdb::DatabaseInstance> owner;
  std::mutex mutex;
  // canonical table key -> the COLUMN FINGERPRINT the installed trigger
  // bodies encode. Not a bare set: a trigger body pins the column list at
  // install time, so `ALTER TABLE ... ADD COLUMN` leaves a body that emits
  // the OLD width while the commit still takes the exact-delta fast path —
  // measured as a view reading 5.0 where SQL read 12.0. The fingerprint is
  // what makes that visible.
  std::unordered_map<std::string, std::string> installed;
  // Tracked tables that are NOT in the catalog right now: a table dropped
  // while still tracked, or one created inside a transaction that rolled
  // back. Counted alongside `installed` so their absence is a steady state
  // rather than a mismatch that re-fires the reconcile on every statement.
  std::unordered_set<std::string> missing;
  std::unordered_set<std::string> sinks; // quoted sink table names
  // Per catalog holding tracked tables, the catalog version last reconciled
  // against. Catalog::GetCatalogVersion is the same signal prepared
  // statements use to invalidate themselves: it moves on any committed
  // catalog change, and jumps above TRANSACTION_START while a transaction has
  // uncommitted catalog changes of its own. That second property is what
  // makes an in-flight `ALTER TABLE ... ADD COLUMN` visible.
  std::unordered_map<std::string, idx_t> catalog_versions;
  // Force the next sweep to do the full reconcile. CLEARED ONLY ON SUCCESS —
  // an earlier version cleared it up front, so any throw (or a deferral)
  // disarmed the one thing that would have retried, and stale bodies stood
  // forever.
  std::atomic<bool> recheck{true};
};

inline std::mutex &trigger_states_mutex() {
  static std::mutex m;
  return m;
}

inline std::map<const void *, std::unique_ptr<TriggerInstallState>> &
trigger_states() {
  static std::map<const void *, std::unique_ptr<TriggerInstallState>> states;
  return states;
}

inline TriggerInstallState &
trigger_install_state(const duckdb::shared_ptr<duckdb::DatabaseInstance> &db) {
  std::lock_guard<std::mutex> g(trigger_states_mutex());
  auto &slot = trigger_states()[db.get()];
  if (!slot || slot->owner.lock().get() != db.get()) {
    slot = std::make_unique<TriggerInstallState>();
    slot->owner = db;
  }
  return *slot;
}

/// MUST be called when a database closes: DuckDB reuses freed
/// DatabaseInstance addresses, so a stale entry would make a brand-new
/// database believe its tables already carry triggers (same law as
/// dbsp_forget_recovery).
inline void dbsp_forget_triggers(const void *db) {
  std::lock_guard<std::mutex> g(trigger_states_mutex());
  trigger_states().erase(db);
}

// ---------------------------------------------------------------------------
// DDL generation
// ---------------------------------------------------------------------------

inline std::string trigger_quote_ident(const std::string &name) {
  std::string out = "\"";
  for (char c : name) {
    if (c == '"') {
      out += "\"\"";
    } else {
      out += c;
    }
  }
  out += "\"";
  return out;
}

// Deterministic trigger name for one table/op, carrying a hash of the table
// key AND the column fingerprint the body was generated from.
//
// The fingerprint is IN THE NAME on purpose: it makes the catalog, not this
// process's memory, the record of what is installed. A fresh process (or a
// reopened database) can then see "these exact bodies are already here" from
// `duckdb_triggers()` alone, instead of re-issuing CREATE OR REPLACE and
// declaring the current transaction untrusted. Measured before that: the first
// write after every reopen discarded its exact delta and ran a full
// scan-and-diff of every tracked table.
//
// It also makes a stale body impossible to mistake for a current one: a
// changed column list produces a different name, so the check is an existence
// test rather than a text comparison against the engine's re-rendered SQL.
inline std::string trigger_name_for(const std::string &table,
                                    const std::string &fingerprint,
                                    const char *op) {
  std::string sanitized;
  for (char c : table) {
    sanitized += (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                     ? c
                     : '_';
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "_%08llx",
           static_cast<unsigned long long>(
               std::hash<std::string>{}(table + "|" + fingerprint) &
               0xffffffffULL));
  return "dbsp_trg_" + sanitized + buf + "_" + op;
}

// Every DBSP trigger name starts with this, whatever the table or fingerprint.
inline const char *trigger_name_prefix() { return "dbsp_trg_"; }

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

// The sink every trigger body writes its one aggregate row into. A trigger
// body MUST be a DML statement (the engine has no procedural body), so the
// ingest scalar rides an `INSERT INTO <sink> SELECT max(...) FROM <transition>`
// — one sink row per statement firing, drained periodically. The sink lives in
// the tracked table's OWN catalog: DuckDB refuses to write two catalogs in one
// transaction, so a sink anywhere else would break every tracked write.
inline std::string sink_name_for(const std::string &catalog,
                                 const std::string &schema) {
  return trigger_quote_ident(catalog) + "." + trigger_quote_ident(schema) +
         "." + trigger_quote_ident("dbsp_trigger_sink");
}

// `dbsp_trigger_ingest('<key>', <weight>, "c1", "c2", ...)`
inline std::string ingest_call(const std::string &key, int weight,
                               const TableSchema &schema) {
  std::string call = "dbsp_trigger_ingest('" + key + "', " +
                     std::to_string(weight);
  for (const auto &col : schema.columns) {
    call += ", " + trigger_quote_ident(col.name);
  }
  call += ")";
  return call;
}

inline std::vector<std::string> trigger_ddl_for(const std::string &key,
                                                const TableSchema &schema,
                                                const std::string &fp) {
  std::string catalog, schema_name, table;
  if (!split_table_key(key, catalog, schema_name, table)) {
    return {};
  }
  const std::string qualified = quote_table_key(key);
  const std::string sink = sink_name_for(catalog, schema_name);
  std::vector<std::string> ddl;
  ddl.push_back("CREATE OR REPLACE TRIGGER " + trigger_name_for(table, fp, "ins") +
                " AFTER INSERT ON " + qualified +
                " REFERENCING NEW TABLE AS dbsp_new FOR EACH STATEMENT"
                " INSERT INTO " +
                sink + " SELECT max(" + ingest_call(key, 1, schema) +
                ") FROM dbsp_new");
  ddl.push_back("CREATE OR REPLACE TRIGGER " + trigger_name_for(table, fp, "del") +
                " AFTER DELETE ON " + qualified +
                " REFERENCING OLD TABLE AS dbsp_old FOR EACH STATEMENT"
                " INSERT INTO " +
                sink + " SELECT max(" + ingest_call(key, -1, schema) +
                ") FROM dbsp_old");
  // Both images in ONE body: the Z-set wants the old row at -1 and the new one
  // at +1 independently, so no join between the transition tables is needed.
  ddl.push_back("CREATE OR REPLACE TRIGGER " + trigger_name_for(table, fp, "upd") +
                " AFTER UPDATE ON " + qualified +
                " REFERENCING OLD TABLE AS dbsp_old NEW TABLE AS dbsp_new"
                " FOR EACH STATEMENT INSERT INTO " +
                sink + " SELECT max(v) FROM (SELECT " +
                ingest_call(key, -1, schema) +
                " AS v FROM dbsp_old UNION ALL SELECT " +
                ingest_call(key, 1, schema) + " AS v FROM dbsp_new)");
  return ddl;
}

// Drop by NAME: the caller reads the live names off duckdb_triggers(), because
// a body generated under an older fingerprint carries an older name and could
// not be reconstructed from the table's current columns.
inline std::vector<std::string>
trigger_drop_ddl_for(const std::string &key,
                     const std::vector<std::string> &names) {
  const std::string qualified = quote_table_key(key);
  std::vector<std::string> ddl;
  ddl.reserve(names.size());
  for (const auto &name : names) {
    ddl.push_back("DROP TRIGGER IF EXISTS " + trigger_quote_ident(name) +
                  " ON " + qualified);
  }
  return ddl;
}

// ---------------------------------------------------------------------------
// Live schema + fingerprint
// ---------------------------------------------------------------------------

// The columns the table has RIGHT NOW, read from the catalog rather than from
// the manager's cached TableSchema. The cache is what the trigger bodies were
// generated from; comparing it against itself would never detect an ALTER.
inline bool live_table_columns(duckdb::ClientContext &context,
                               const std::string &key, TableSchema &out) {
  auto entry = resolve_table_entry(context, key);
  if (!entry) {
    return false; // dropped, renamed, or in a detached catalog
  }
  out.table_name = key;
  out.columns.clear();
  for (auto &col : entry->GetColumns().Logical()) {
    ColumnInfo info;
    info.name = col.Name().GetIdentifierName();
    info.type = col.Type();
    out.columns.push_back(info);
  }
  return !out.columns.empty();
}

// Name and type of every column, in order — everything a generated body
// depends on. A change here means the installed bodies are stale.
inline std::string schema_fingerprint(const TableSchema &schema) {
  std::string fp;
  for (const auto &col : schema.columns) {
    fp += col.name;
    fp += ':';
    fp += col.type.ToString();
    fp += '|';
  }
  return fp;
}

/// A SQL string literal with quotes doubled.
inline std::string sql_literal(const std::string &v) {
  std::string out = "'";
  for (char c : v) {
    if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  return out + "'";
}

/// Does the sink table for this catalog/schema already exist? Asked on the
/// internal connection so it reflects a fresh snapshot; skipping the CREATE
/// when it does is what keeps the common sweep out of the catalog-write path
/// altogether (see the write-write conflict note in install_pending_triggers).
inline bool sink_exists(duckdb::Connection &con, const std::string &catalog,
                        const std::string &schema) {
  auto r = con.Query(
      "SELECT count(*) FROM duckdb_tables() WHERE database_name = " +
      sql_literal(catalog) + " AND schema_name = " + sql_literal(schema) +
      " AND table_name = 'dbsp_trigger_sink'");
  if (r->HasError() || r->RowCount() == 0) {
    return false;
  }
  return r->GetValue(0, 0).GetValue<int64_t>() > 0;
}

/// Every DBSP trigger currently in the catalog, as
/// "catalog.schema.table" -> [trigger names].
///
/// Read on an internal connection, which is safe here in a way that DDL is
/// not: this is a plain READ, so it takes its own snapshot instead of trying
/// to see (or fight with) the user's uncommitted catalog changes. It is paid
/// only when the process's own install record disagrees with the tracked set —
/// once per database in the steady state.
///
/// duckdb_triggers() is the route because trigger entries are not reachable
/// through Catalog::GetEntry: the generic entry lookup returns nothing for
/// CatalogType::TRIGGER_ENTRY even when the trigger is right there in
/// duckdb_triggers() (measured on v2.0.0-alpha39998).
inline std::unordered_map<std::string, std::vector<std::string>>
read_dbsp_triggers(duckdb::Connection &con) {
  std::unordered_map<std::string, std::vector<std::string>> out;
  auto r = con.Query(
      "SELECT database_name, schema_name, table_name, trigger_name "
      "FROM duckdb_triggers() "
      "WHERE trigger_name LIKE 'dbsp\\_trg\\_%' ESCAPE '\\'");
  if (r->HasError()) {
    throw duckdb::InvalidInputException(
        "DBSP trigger source: could not read duckdb_triggers(): %s",
        r->GetError());
  }
  for (duckdb::idx_t i = 0; i < r->RowCount(); i++) {
    out[r->GetValue(0, i).ToString() + "." + r->GetValue(1, i).ToString() +
        "." + r->GetValue(2, i).ToString()]
        .push_back(r->GetValue(3, i).ToString());
  }
  return out;
}

/// Are the three bodies for `key` at column fingerprint `fp` already present?
/// The fingerprint is part of the trigger NAME (trigger_name_for), so presence
/// alone proves the bodies match the table's current columns — no comparison
/// against the engine's re-rendered trigger SQL, which normalises quoting and
/// clause order and could not be compared reliably.
inline bool triggers_present(
    const std::unordered_map<std::string, std::vector<std::string>> &live,
    const std::string &key, const std::string &table, const std::string &fp) {
  auto it = live.find(key);
  if (it == live.end()) {
    return false;
  }
  for (const char *op : {"ins", "del", "upd"}) {
    const std::string want = trigger_name_for(table, fp, op);
    if (std::find(it->second.begin(), it->second.end(), want) ==
        it->second.end()) {
      return false;
    }
  }
  return true;
}

/// True for a statement the sweep must keep its hands off entirely.
///
/// Reading a catalog's version (catalog_version_of, below) calls
/// Transaction::Get for that catalog, which JOINS it to the statement's
/// transaction. `DETACH` refuses to run when the transaction has any
/// outstanding work on the target database
/// (duckdb/src/execution/operator/schema/physical_detach.cpp:20-30), so the
/// sweep's own gate made DETACH impossible for every catalog holding a tracked
/// table. Measured: `DETACH b` threw "Cannot detach database b because the
/// current transaction has outstanding work on it - commit or rollback first"
/// on a connection that had only ever read from b.
///
/// A leading-keyword sniff is the right instrument HERE and the wrong one for
/// detecting DDL: a miss costs that same loud error, never silent staleness.
/// The sweep is skipped for this one statement and `recheck` still stands.
inline bool statement_detaches(const std::string &query) {
  const auto first = query.find_first_not_of(" \t\r\n(");
  if (first == std::string::npos) {
    return false;
  }
  return duckdb::StringUtil::Lower(query.substr(first, 6)) == "detach";
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
/// statement — including ROLLBACK — after a CREATE + track in one transaction.
inline bool user_transaction_open(duckdb::ClientContext &context) {
  return context.transaction.HasActiveTransaction() &&
         !context.transaction.IsAutoCommit();
}

/// The catalog's current version, or false when the catalog does not support
/// versioning (a non-DuckDB attached catalog). A catalog that answers false is
/// never RECORDED (see the end of install_pending_triggers), so it is skipped
/// by catalogs_moved rather than fingerprinted per statement.
inline bool catalog_version_of(duckdb::ClientContext &context,
                               const std::string &catalog_name, idx_t &out) {
  try {
    auto &catalog = duckdb::Catalog::GetCatalog(
        context, duckdb::Identifier(catalog_name));
    auto version = catalog.GetCatalogVersion(context);
    if (!version.IsValid()) {
      return false;
    }
    out = version.GetIndex();
    return true;
  } catch (...) {
    return false;
  }
}

/// Has any catalog holding tracked tables changed since the last reconcile?
/// This replaces an earlier leading-keyword sniff of the statement text, which
/// could not see DDL that did not arrive as text and could not see a
/// transaction's own uncommitted changes at all.
///
/// Only catalogs the last reconcile RECORDED a version for are checked, and a
/// catalog whose version could not be read is never recorded — so the
/// "unversioned" branch below is a belt-and-braces guard, not a live path.
/// Every attached DuckDB file is a DuckCatalog and answers GetCatalogVersion;
/// a catalog that is not one cannot host the triggers at all, and the install
/// throws there rather than going quietly stale.
inline bool catalogs_moved(duckdb::ClientContext &context,
                           TriggerInstallState &st) {
  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> g(st.mutex);
    names.reserve(st.catalog_versions.size());
    for (const auto &entry : st.catalog_versions) {
      names.push_back(entry.first);
    }
  }
  for (const auto &name : names) {
    idx_t version = 0;
    if (!catalog_version_of(context, name, version)) {
      return true; // unversioned catalog: never assume it stood still
    }
    std::lock_guard<std::mutex> g(st.mutex);
    auto it = st.catalog_versions.find(name);
    if (it == st.catalog_versions.end() || it->second != version) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Install / reconcile
// ---------------------------------------------------------------------------

// Bring the triggers in the catalog in line with the manager's tracked-table
// set. Runs on its OWN connection (an internal one, so DBSP's hooks ignore it)
// and therefore in its own transaction: the statement that triggers this call
// has already begun and will NOT see the new triggers, which is why the caller
// records "triggers just installed" and lets that transaction reconcile by
// scan. Every transaction that begins afterwards sees them.
//
// Returns true when it created or dropped anything. Throws on failure — a
// tracked table with no trigger is silent staleness, and silence is the one
// outcome this source must never have.
/// Outcome of one sweep. DEFERRED is the important one: work IS needed but
/// the user holds a transaction open, so the transaction's commit must
/// reconcile by scan and the DDL waits until the transaction ends.
enum class ReconcileResult { UNCHANGED, CHANGED, DEFERRED };

inline ReconcileResult install_pending_triggers(duckdb::ClientContext &context,
                                                CDCManager &manager) {
  auto &st = trigger_install_state(context.db);

  // ---- gate -------------------------------------------------------------
  // Three cheap signals, none of which runs SQL: an armed recheck, a moved
  // tracked-table count, or a moved catalog version. The count alone was not
  // enough — an ALTER never changes it — and the catalog version is what
  // closes that hole, including for a transaction's own uncommitted DDL.
  const bool armed = st.recheck.load(std::memory_order_relaxed);
  bool count_moved;
  {
    std::lock_guard<std::mutex> g(st.mutex);
    count_moved =
        manager.tracked_table_total() != st.installed.size() + st.missing.size();
  }
  if (!armed && !count_moved && !catalogs_moved(context, st)) {
    return ReconcileResult::UNCHANGED;
  }

  // ---- what needs doing, computed WITHOUT SQL ---------------------------
  // Every lookup below runs on the CALLER's context, so it sees exactly what
  // the caller sees — including their own uncommitted DDL — and opens no
  // internal connection. That matters twice over: an internal connection
  // cannot see uncommitted catalog changes at all, and running one while the
  // user holds a transaction open is what wedged their connection.
  const std::vector<std::string> keys = manager.list_tracked_tables();
  const std::unordered_set<std::string> tracked(keys.begin(), keys.end());

  struct Pending {
    std::string key;
    TableSchema live;
    std::string fingerprint;
  };
  struct Candidate {
    std::string key;
    std::string table;
    TableSchema live;
    std::string fingerprint;
  };
  std::vector<Candidate> unproven; // process record disagrees; ask the catalog
  std::vector<Pending> to_install;
  std::vector<std::string> now_missing;
  std::vector<std::string> back_from_missing;
  std::set<std::string> catalogs;

  for (const auto &key : keys) {
    std::string catalog, schema_name, table;
    if (!split_table_key(key, catalog, schema_name, table)) {
      throw duckdb::InvalidInputException(
          "DBSP trigger source: table key '%s' is not catalog.schema.table",
          key);
    }
    catalogs.insert(catalog);
    TableSchema live;
    if (!live_table_columns(context, key, live)) {
      // Tracked but not in the catalog from here: dropped, or created inside a
      // transaction that rolled back. Recorded rather than retried forever —
      // and never a throw, because a throw here reached the user on every
      // statement including their ROLLBACK.
      now_missing.push_back(key);
      continue;
    }
    const std::string fp = schema_fingerprint(live);
    {
      std::lock_guard<std::mutex> g(st.mutex);
      if (st.missing.count(key)) {
        back_from_missing.push_back(key);
      }
      auto it = st.installed.find(key);
      if (it != st.installed.end() && it->second == fp) {
        continue; // this process installed exactly these bodies
      }
    }
    unproven.push_back({key, table, std::move(live), fp});
  }

  // The CATALOG is the record, not this process's memory. Trigger names carry
  // the column fingerprint, so "the three names exist" IS "the right bodies
  // are installed". Before this, a reopened database (empty install record,
  // bodies already in the catalog) re-issued CREATE OR REPLACE on the first
  // statement — and because that DDL commits on an internal connection AFTER
  // the statement took its catalog snapshot, the sweep had to mark the
  // transaction untrusted, making the FIRST WRITE AFTER EVERY REOPEN a full
  // scan-and-diff of every tracked table.
  //
  // The read is safe where the DDL is not: it takes its own snapshot instead
  // of trying to see the user's uncommitted catalog changes. It is paid only
  // when this process's install record disagrees with the tracked set — once
  // per database in the steady state.
  if (!unproven.empty()) {
    InternalQueryGuard guard;
    duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
    const auto live_triggers = read_dbsp_triggers(con);
    for (auto &c : unproven) {
      if (triggers_present(live_triggers, c.key, c.table, c.fingerprint)) {
        std::lock_guard<std::mutex> g(st.mutex);
        st.installed[c.key] = c.fingerprint;
        st.missing.erase(c.key);
        continue;
      }
      to_install.push_back({c.key, std::move(c.live), c.fingerprint});
    }
  }

  std::vector<std::string> to_drop;
  {
    std::lock_guard<std::mutex> g(st.mutex);
    for (const auto &entry : st.installed) {
      if (!tracked.count(entry.first)) {
        to_drop.push_back(entry.first);
      }
    }
  }

  // ---- defer while the user's transaction is open -----------------------
  // Nothing above opened a connection or ran DDL, which is the point: the
  // internal connection cannot see uncommitted catalog changes, so touching it
  // here is what made a user's COMMIT fail and wedged a connection that had
  // created and tracked a table in one transaction. The caller poisons this
  // transaction instead, so its commit reconciles by scan, and `recheck` stays
  // armed for the first statement after the transaction ends.
  const bool work_pending =
      !to_install.empty() || !to_drop.empty() || !now_missing.empty();
  if (user_transaction_open(context)) {
    if (work_pending) {
      st.recheck.store(true, std::memory_order_relaxed);
      return ReconcileResult::DEFERRED;
    }
    // Nothing to do — and the bookkeeping above is pure memory, so it is safe
    // to bank it and stop re-deriving it on every statement of a long
    // transaction. `armed` alone is not a reason to defer any more.
  }

  // ---- act --------------------------------------------------------------
  //
  // Everything below runs DDL on an internal connection, i.e. in a transaction
  // of its own, concurrently with whatever the user's connection is doing. A
  // CATALOG WRITE-WRITE CONFLICT is therefore an ordinary outcome, not a
  // failure: DuckDB raises it when two transactions create entries in the same
  // schema at once, and `CREATE TABLE IF NOT EXISTS` is not atomic against a
  // concurrent creator. Measured: NumPad builds several materialized views in
  // a row on a worker thread, and the sweep's sink CREATE lost that race and
  // failed the USER's statement with
  // `Catalog write-write conflict on create with "Schema main Table
  // dbsp_trigger_sink"`.
  //
  // A conflict is retried, not swallowed and not thrown: `recheck` stays armed
  // and the caller is told DEFERRED, so this transaction's commit reconciles
  // by scan and the next statement installs. Correct answer, no user-visible
  // failure, and still no silence. Every other DDL error still throws.
  bool changed = false;
  if (!to_install.empty() || !to_drop.empty()) {
    InternalQueryGuard guard;
    duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));

    // The DBSP triggers already on the tables we are about to touch. Needed
    // because a body generated under an OLDER fingerprint carries an older
    // name: CREATE OR REPLACE would leave it in place, and it would go on
    // delivering rows of the wrong width alongside the new one.
    std::unordered_map<std::string, std::vector<std::string>> existing;
    {
      auto r = con.Query(
          "SELECT database_name, schema_name, table_name, trigger_name "
          "FROM duckdb_triggers() "
          "WHERE trigger_name LIKE 'dbsp\\_trg\\_%' ESCAPE '\\'");
      if (r->HasError()) {
        throw duckdb::InvalidInputException(
            "DBSP trigger source: could not read duckdb_triggers(): %s",
            r->GetError());
      }
      for (duckdb::idx_t i = 0; i < r->RowCount(); i++) {
        existing[r->GetValue(0, i).ToString() + "." +
                 r->GetValue(1, i).ToString() + "." +
                 r->GetValue(2, i).ToString()]
            .push_back(r->GetValue(3, i).ToString());
      }
    }

    std::unordered_set<std::string> made_sinks;
    auto install_one = [&](const Pending &p) {
      std::string catalog, schema_name, table;
      split_table_key(p.key, catalog, schema_name, table);
      const std::string sink = sink_name_for(catalog, schema_name);
      if (made_sinks.insert(sink).second && !sink_exists(con, catalog, schema_name)) {
        auto r = con.Query("CREATE TABLE IF NOT EXISTS " + sink + "(v BIGINT)");
        if (r->HasError()) {
          throw duckdb::InvalidInputException(
              "DBSP trigger source: could not create sink %s: %s", sink,
              r->GetError());
        }
      }
      // Drop every DBSP trigger on this table that is not one of the three we
      // are about to create — i.e. the bodies of a previous fingerprint.
      std::unordered_set<std::string> wanted;
      for (const char *op : {"ins", "del", "upd"}) {
        wanted.insert(trigger_name_for(table, p.fingerprint, op));
      }
      std::vector<std::string> stale;
      auto it = existing.find(p.key);
      if (it != existing.end()) {
        for (const auto &name : it->second) {
          if (!wanted.count(name)) {
            stale.push_back(name);
          }
        }
      }
      for (const auto &sql : trigger_drop_ddl_for(p.key, stale)) {
        auto r = con.Query(sql);
        if (r->HasError()) {
          throw duckdb::InvalidInputException(
              "DBSP trigger source: could not drop a stale body: %s [%s]",
              r->GetError(), sql);
        }
      }
      for (const auto &sql : trigger_ddl_for(p.key, p.live, p.fingerprint)) {
        auto r = con.Query(sql);
        if (r->HasError()) {
          throw duckdb::InvalidInputException("DBSP trigger source: %s [%s]",
                                              r->GetError(), sql);
        }
      }
      std::lock_guard<std::mutex> g(st.mutex);
      st.installed[p.key] = p.fingerprint;
      st.missing.erase(p.key);
      st.sinks.insert(sink);
    };

    try {
      for (const auto &p : to_install) {
        install_one(p);
        changed = true;
      }
    } catch (const std::exception &e) {
      if (std::string(e.what()).find("write-write conflict") ==
          std::string::npos) {
        throw;
      }
      st.recheck.store(true, std::memory_order_relaxed);
      return ReconcileResult::DEFERRED;
    }
    for (const auto &key : to_drop) {
      auto it = existing.find(key);
      if (it != existing.end()) {
        for (const auto &sql : trigger_drop_ddl_for(key, it->second)) {
          con.Query(sql); // best effort: the table itself may already be gone
        }
      }
      std::lock_guard<std::mutex> g(st.mutex);
      st.installed.erase(key);
      st.missing.erase(key);
      changed = true;
    }
  }

  {
    std::lock_guard<std::mutex> g(st.mutex);
    for (const auto &key : now_missing) {
      if (st.installed.erase(key)) {
        changed = true;
      }
      st.missing.insert(key);
    }
    for (const auto &key : back_from_missing) {
      st.missing.erase(key);
    }
    // Only the catalogs we just reconciled against are recorded, and only now:
    // recording them earlier would have let a failed pass look complete.
    st.catalog_versions.clear();
    for (const auto &name : catalogs) {
      idx_t version = 0;
      if (catalog_version_of(context, name, version)) {
        st.catalog_versions[name] = version;
      }
    }
  }
  // SUCCESS is the only path that disarms. A throw above leaves it armed, so
  // the next statement retries instead of standing on stale bodies.
  st.recheck.store(false, std::memory_order_relaxed);
  return changed ? ReconcileResult::CHANGED : ReconcileResult::UNCHANGED;
}

// One sink row accumulates per statement firing. Nothing ever reads them, so
// they are drained periodically to keep a long-lived process from growing the
// sink without bound.
//
// The sinks it drains are the ones the CATALOG reports, not the ones this
// process installed: triggers are catalog objects, so a database tracked by an
// earlier process arrives with bodies already writing sink rows, and a drain
// keyed on this process's own install record would never reclaim those.
inline void maybe_drain_trigger_sinks(duckdb::ClientContext &context) {
  const uint64_t every = trigger_sink_drain_interval();
  if (trigger_commits_total().fetch_add(1, std::memory_order_relaxed) + 1 <
      every) {
    return; // one relaxed atomic on the common commit path, nothing else
  }
  trigger_commits_total().store(0, std::memory_order_relaxed);
  try {
    InternalQueryGuard guard;
    duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
    // Ask the catalog which sinks exist rather than trusting an install
    // record: in a non-trigger mode this process never installed anything and
    // has no record, yet the sinks are there and filling.
    auto r = con.Query("SELECT DISTINCT database_name, schema_name "
                       "FROM duckdb_triggers() "
                       "WHERE trigger_name LIKE 'dbsp\\_trg\\_%' ESCAPE '\\'");
    if (r->HasError()) {
      return;
    }
    for (duckdb::idx_t i = 0; i < r->RowCount(); i++) {
      const std::string sink = sink_name_for(r->GetValue(0, i).ToString(),
                                             r->GetValue(1, i).ToString());
      con.Query("DELETE FROM " + sink);
    }
  } catch (...) {
    // Housekeeping only: a failed drain costs disk, never correctness.
  }
}

} // namespace dbsp_native
