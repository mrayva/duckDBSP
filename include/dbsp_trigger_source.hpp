#pragma once
// Trigger-fed delta source (spike). A third way to learn what a committing
// transaction wrote, alongside the SaaS engine hook (dbsp_engine_hook.hpp)
// and the predictive capture stack (dbsp_write_capture.hpp + dbsp_plan_tee.hpp).
//
// The idea: DuckDB 2.0 has statement-level AFTER triggers with transition
// tables. `dbsp_track(t)` generates three of them on `t`
//
//   CREATE OR REPLACE TRIGGER dbsp_trg_<t>_ins AFTER INSERT ON <t>
//     REFERENCING NEW TABLE AS dbsp_new FOR EACH STATEMENT
//     INSERT INTO <sink> SELECT max(dbsp_trigger_ingest('<key>', 1, <cols>))
//                        FROM dbsp_new;
//
// (plus the DELETE/-1 and the UPDATE one, which carries BOTH images through a
// UNION ALL). The body is SQL — that is all the engine offers, there is no C++
// trigger API — so the row images reach C++ through a vectorised, volatile
// extension scalar that writes them into the SAME per-transaction buffer the
// engine hook fills (DBSPContextState::engine_buffer_delta), tagged with the
// canonical catalog.schema.table key and weight (-1 old image, +1 new image).
// TransactionCommit then applies them through the one existing ingest path, so
// the CDC core (dbsp_cdc.hpp) is untouched, and TransactionRollback discards
// them for free.
//
// Why it matters: trigger expansion is binder-level, and in 2.0 even the
// Appender goes through the binder (Appender::FlushInternal runs an
// INSERT ... SELECT). So this source needs NO patched engine — it is exact
// deltas on a STOCK DuckDB. What it costs is `MERGE INTO`, which the engine
// rejects outright on any table carrying a trigger.
//
// Mode is chosen by the DBSP_DELTA_SOURCE environment variable, read once at
// extension load: `trigger` | `hook` | `capture`; unset keeps today's
// behaviour (hook if the build and engine have it, capture otherwise).
//
// See docs/DESIGN_TRIGGER_SOURCE.md.

#include "dbsp_cdc.hpp"
#include "dbsp_qualified_name.hpp"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace dbsp_native {

// ---------------------------------------------------------------------------
// Mode switch
// ---------------------------------------------------------------------------

enum class DeltaSource { DEFAULT, HOOK, CAPTURE, TRIGGER };

// Read ONCE, at first use. The benchmark host (NumPad) cannot run SQL before
// its own code opens the database, so an environment variable — not a SET — is
// the load-bearing switch. `dbsp_stats()` reports the resulting mode so it is
// verifiable from the host.
inline DeltaSource delta_source() {
  static const DeltaSource mode = [] {
    const char *v = std::getenv("DBSP_DELTA_SOURCE");
    if (!v || !*v) {
      return DeltaSource::DEFAULT;
    }
    if (std::strcmp(v, "trigger") == 0) {
      return DeltaSource::TRIGGER;
    }
    if (std::strcmp(v, "hook") == 0) {
      return DeltaSource::HOOK;
    }
    if (std::strcmp(v, "capture") == 0) {
      return DeltaSource::CAPTURE;
    }
    // Fail loud rather than silently running a mode the caller did not ask
    // for: a typo here would otherwise be invisible in a benchmark.
    std::cerr << "DBSP: unknown DBSP_DELTA_SOURCE='" << v
              << "' (expected trigger|hook|capture) — using the default\n";
    return DeltaSource::DEFAULT;
  }();
  return mode;
}

inline bool trigger_source_enabled() {
  return delta_source() == DeltaSource::TRIGGER;
}

// PROOF OF LIFE, the same contract the engine hook keeps: the flag that
// disarms the capture stack flips only when a trigger body has actually
// delivered rows, never at install time. A trigger that never fires leaves
// the capture stack armed instead of silently dropping every commit.
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
// touches the per-database map (see maybe_drain_trigger_sinks).
inline std::atomic<uint64_t> &trigger_firings_total() {
  static std::atomic<uint64_t> n{0};
  return n;
}

// ---------------------------------------------------------------------------
// Row images: chunk columns [first_col, end) -> signed Z-set
// ---------------------------------------------------------------------------

// Vectorised, mirroring engine_cdc_to_zset (dbsp_engine_hook.hpp): read typed
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
  std::unordered_set<std::string> sinks; // quoted sink table names
  std::atomic<uint64_t> firings_since_drain{0};
  // Force the next sweep to do the full catalog reconcile rather than trust
  // the tracked-table count. Armed after any DDL statement, because DDL is
  // exactly what a count cannot see: ADD COLUMN changes a schema, DROP TABLE
  // takes the triggers with it, and neither moves the count by one.
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

// Deterministic, collision-free trigger name for one table/op. Tracked table
// names are usually plain identifiers (dbsp_track validates its argument), but
// view-source auto-tracking can reach a quoted name — those get sanitised and
// disambiguated by a hash of the original so two odd names cannot collide.
inline std::string trigger_name_for(const std::string &table, const char *op) {
  std::string sanitized;
  bool changed = false;
  for (char c : table) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      sanitized += c;
    } else {
      sanitized += '_';
      changed = true;
    }
  }
  std::string name = "dbsp_trg_" + sanitized;
  if (changed) {
    char buf[20];
    snprintf(buf, sizeof(buf), "_%08llx",
             static_cast<unsigned long long>(std::hash<std::string>{}(table) &
                                             0xffffffffULL));
    name += buf;
  }
  return name + "_" + op;
}

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
                                                const TableSchema &schema) {
  std::string catalog, schema_name, table;
  if (!split_table_key(key, catalog, schema_name, table)) {
    return {};
  }
  const std::string qualified = quote_table_key(key);
  const std::string sink = sink_name_for(catalog, schema_name);
  std::vector<std::string> ddl;
  ddl.push_back("CREATE OR REPLACE TRIGGER " + trigger_name_for(table, "ins") +
                " AFTER INSERT ON " + qualified +
                " REFERENCING NEW TABLE AS dbsp_new FOR EACH STATEMENT"
                " INSERT INTO " +
                sink + " SELECT max(" + ingest_call(key, 1, schema) +
                ") FROM dbsp_new");
  ddl.push_back("CREATE OR REPLACE TRIGGER " + trigger_name_for(table, "del") +
                " AFTER DELETE ON " + qualified +
                " REFERENCING OLD TABLE AS dbsp_old FOR EACH STATEMENT"
                " INSERT INTO " +
                sink + " SELECT max(" + ingest_call(key, -1, schema) +
                ") FROM dbsp_old");
  // Both images in ONE body: the Z-set wants the old row at -1 and the new one
  // at +1 independently, so no join between the transition tables is needed.
  ddl.push_back("CREATE OR REPLACE TRIGGER " + trigger_name_for(table, "upd") +
                " AFTER UPDATE ON " + qualified +
                " REFERENCING OLD TABLE AS dbsp_old NEW TABLE AS dbsp_new"
                " FOR EACH STATEMENT INSERT INTO " +
                sink + " SELECT max(v) FROM (SELECT " +
                ingest_call(key, -1, schema) +
                " AS v FROM dbsp_old UNION ALL SELECT " +
                ingest_call(key, 1, schema) + " AS v FROM dbsp_new)");
  return ddl;
}

inline std::vector<std::string> trigger_drop_ddl_for(const std::string &key) {
  std::string catalog, schema_name, table;
  if (!split_table_key(key, catalog, schema_name, table)) {
    return {};
  }
  const std::string qualified = quote_table_key(key);
  std::vector<std::string> ddl;
  for (const char *op : {"ins", "del", "upd"}) {
    ddl.push_back("DROP TRIGGER IF EXISTS " + trigger_name_for(table, op) +
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

/// Arm a full catalog reconcile on this database's next statement.
inline void request_trigger_recheck(
    const duckdb::shared_ptr<duckdb::DatabaseInstance> &db) {
  if (!trigger_source_enabled()) {
    return;
  }
  trigger_install_state(db).recheck.store(true, std::memory_order_relaxed);
}

/// Cheap first-keyword test: is this statement DDL, i.e. could it have moved
/// a tracked table's schema or taken its triggers away? Deliberately
/// over-inclusive — a false positive costs one catalog reconcile, a false
/// negative costs silent wrong answers.
inline bool looks_like_ddl(const std::string &sql) {
  size_t i = 0;
  while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) {
    i++;
  }
  const char *kw[] = {"alter", "drop", "create", "attach", "detach"};
  for (const char *k : kw) {
    const size_t n = std::strlen(k);
    if (sql.size() - i >= n &&
        duckdb::StringUtil::CIEquals(sql.substr(i, n), k)) {
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
inline bool install_pending_triggers(duckdb::ClientContext &context,
                                     CDCManager &manager) {
  if (!trigger_source_enabled()) {
    return false;
  }
  auto &st = trigger_install_state(context.db);
  const bool forced = st.recheck.exchange(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> g(st.mutex);
    if (!forced && manager.tracked_table_total() == st.installed.size()) {
      return false; // steady state: no DDL since the last pass, nothing new
    }
  }
  const std::vector<std::string> keys = manager.list_tracked_tables();
  const std::unordered_set<std::string> tracked(keys.begin(), keys.end());

  InternalQueryGuard guard;
  duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));

  // What the catalog ACTUALLY holds. Counting tracked tables cannot see a
  // DROP TABLE (which takes the triggers with it) followed by a recreate and
  // a re-track: the count comes back to where it was and the table is left
  // permanently triggerless. One query answers it for every table at once.
  std::unordered_set<std::string> live_triggers; // "catalog.schema.name"
  {
    auto r = con.Query("SELECT database_name, schema_name, trigger_name "
                       "FROM duckdb_triggers()");
    if (r->HasError()) {
      throw duckdb::InvalidInputException(
          "DBSP trigger source: could not read duckdb_triggers(): %s",
          r->GetError());
    }
    for (duckdb::idx_t i = 0; i < r->RowCount(); i++) {
      live_triggers.insert(r->GetValue(0, i).ToString() + "." +
                           r->GetValue(1, i).ToString() + "." +
                           r->GetValue(2, i).ToString());
    }
  }

  bool changed = false;
  std::unordered_set<std::string> made_sinks;
  for (const auto &key : keys) {
    std::string catalog, schema_name, table;
    if (!split_table_key(key, catalog, schema_name, table)) {
      throw duckdb::InvalidInputException(
          "DBSP trigger source: table key '%s' is not catalog.schema.table",
          key);
    }
    // The LIVE columns, not the manager's cached ones: the cache is what the
    // installed bodies were generated from, so comparing it with itself could
    // never detect an ALTER.
    TableSchema live;
    if (!live_table_columns(context, key, live)) {
      // Tracked but not in the catalog right now (dropped, or its catalog
      // detached). Forget it rather than fail: if it comes back, the next
      // reconcile installs fresh triggers on it.
      std::lock_guard<std::mutex> g(st.mutex);
      if (st.installed.erase(key)) {
        changed = true;
      }
      continue;
    }
    const std::string fp = schema_fingerprint(live);

    bool need = false;
    {
      std::lock_guard<std::mutex> g(st.mutex);
      auto it = st.installed.find(key);
      need = (it == st.installed.end()) || (it->second != fp);
    }
    if (!need) {
      for (const char *op : {"ins", "del", "upd"}) {
        if (!live_triggers.count(catalog + "." + schema_name + "." +
                                 trigger_name_for(table, op))) {
          need = true; // the catalog lost them (DROP TABLE, manual DROP)
          break;
        }
      }
    }
    if (!need) {
      continue;
    }

    const std::string sink = sink_name_for(catalog, schema_name);
    if (made_sinks.insert(sink).second) {
      auto r = con.Query("CREATE TABLE IF NOT EXISTS " + sink + "(v BIGINT)");
      if (r->HasError()) {
        throw duckdb::InvalidInputException(
            "DBSP trigger source: could not create sink %s: %s", sink,
            r->GetError());
      }
    }
    // CREATE OR REPLACE rewrites a stale body in place, so no drop is needed
    // for the ALTER case; the drop only matters if a name ever changes.
    for (const auto &sql : trigger_ddl_for(key, live)) {
      auto r = con.Query(sql);
      if (r->HasError()) {
        throw duckdb::InvalidInputException("DBSP trigger source: %s [%s]",
                                            r->GetError(), sql);
      }
    }
    {
      std::lock_guard<std::mutex> g(st.mutex);
      st.installed[key] = fp;
      st.sinks.insert(sink);
    }
    changed = true;
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
  for (const auto &key : to_drop) {
    for (const auto &sql : trigger_drop_ddl_for(key)) {
      con.Query(sql); // best effort: the table itself may already be gone
    }
    std::lock_guard<std::mutex> g(st.mutex);
    st.installed.erase(key);
    changed = true;
  }
  return changed;
}

// One sink row accumulates per statement firing. Drain them periodically so a
// long-lived process cannot grow the sink without bound; the rows are write-
// only bookkeeping, nothing ever reads them.
inline void maybe_drain_trigger_sinks(duckdb::ClientContext &context) {
  static constexpr uint64_t kDrainEvery = 50000;
  if (!trigger_source_enabled()) {
    return;
  }
  // The counter is consulted through a process-wide atomic BEFORE the
  // per-database map is touched. Going to trigger_install_state() first would
  // take a global mutex and do a map insert on EVERY commit, and would
  // resurrect an entry for a database that dbsp_forget_triggers() had just
  // pruned at close.
  if (trigger_firings_total().load(std::memory_order_relaxed) < kDrainEvery) {
    return;
  }
  auto &st = trigger_install_state(context.db);
  if (st.firings_since_drain.load(std::memory_order_relaxed) < kDrainEvery) {
    return;
  }
  trigger_firings_total().store(0, std::memory_order_relaxed);
  st.firings_since_drain.store(0, std::memory_order_relaxed);
  std::vector<std::string> sinks;
  {
    std::lock_guard<std::mutex> g(st.mutex);
    sinks.assign(st.sinks.begin(), st.sinks.end());
  }
  try {
    InternalQueryGuard guard;
    duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
    for (const auto &sink : sinks) {
      con.Query("DELETE FROM " + sink);
    }
  } catch (...) {
    // Housekeeping only: a failed drain costs disk, never correctness.
  }
}

} // namespace dbsp_native
