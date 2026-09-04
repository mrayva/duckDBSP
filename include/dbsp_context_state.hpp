// Per-connection hook state for auto-CDC.
//
// One delta source: the statement triggers generated on every tracked table
// (dbsp_trigger_source.hpp). Their bodies call `dbsp_trigger_ingest`, which
// buffers the exact old/new row images of the writing transaction here
// (buffer_trigger_delta). TransactionCommit then applies the buffer through
// the CDC core's one ingest path in a single propagation pass, and
// TransactionRollback discards it.
//
// This class owns the four things that cannot live in the trigger bodies:
//
//   * QueryBegin — the trigger sweep (bring the catalog's triggers in line
//     with the tracked-table set) and deferred-baseline materialization;
//   * statement classification — which tracked tables a transaction wrote, so
//     a commit that cannot be served by an exact delta scans only those;
//   * TransactionCommit — the one place where running SQL is safe, so the
//     buffered delta is applied here, never inside a trigger body;
//   * the scan-and-diff fallback, which is the safety net whenever the
//     buffered picture is or might be incomplete (mark_delta_unknown, a
//     trigger install under the transaction, a write nothing accounted for).
//
// Correctness never depends on the trigger path having fired: every route out
// of "I do not know what this transaction wrote" ends in a scan.

#pragma once

#include "dbsp_cdc.hpp"
#include "dbsp_recovery.hpp"
#include "dbsp_trigger_source.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
// DuckDB 2.0: UpdateStatement only forward-declares its UpdateQueryNode
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/transaction/transaction_context.hpp"
#include "duckdb/storage/storage_manager.hpp"

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dbsp_native {

/// Tracks which DatabaseInstances have already run recovery, so it runs
/// once per DATABASE rather than once per process.
///
/// Entries MUST be dropped when a database closes (dbsp_forget_recovery,
/// called from the teardown hook): DuckDB reuses freed DatabaseInstance
/// addresses, so a stale entry makes a BRAND-NEW database at the same
/// address skip recovery entirely — no crash marker, no session registered.
/// Measured: opening three databases, closing them, then opening a fourth
/// left the fourth completely unprotected.
inline std::set<const void *> &dbsp_recovered_dbs() {
  static std::set<const void *> recovered;
  return recovered;
}

inline std::mutex &dbsp_recovered_mutex() {
  static std::mutex m;
  return m;
}

/// True if THIS call is the one that should run recovery for `db`.
inline bool dbsp_claim_recovery(const void *db) {
  std::lock_guard<std::mutex> guard(dbsp_recovered_mutex());
  return dbsp_recovered_dbs().insert(db).second;
}

/// Let a future database at this address recover. Idempotent.
inline void dbsp_forget_recovery(const void *db) {
  std::lock_guard<std::mutex> guard(dbsp_recovered_mutex());
  dbsp_recovered_dbs().erase(db);
}

class DBSPContextState : public duckdb::ClientContextState {
public:
  // ---- trigger-source surface (dbsp_trigger_source.hpp) ---------------
  // Called from a generated trigger body, on an execution thread, while the
  // writing statement runs. Buffer only — application happens in
  // TransactionCommit, where running SQL is safe. The caller has already
  // filtered untracked tables.
  void buffer_trigger_delta(const std::string &table_key, DuckDBZSet &&delta) {
    std::lock_guard<std::mutex> guard(trigger_buffer_mutex_);
    capture_.trigger_fed = true;
    if (delta.empty()) {
      return;
    }
    auto &dst = capture_.trigger_deltas[table_key];
    if (dst.empty()) {
      dst = std::move(delta);
      return;
    }
    for (const auto &[row, w] : delta) {
      dst.insert(row, w);
    }
  }

  // The buffered picture is incomplete — a conversion failed, or a firing
  // could not reach the buffer at all. The commit must reconcile by scan
  // instead of applying a partial delta. Poison, never silence: this is the
  // only route by which "there was nothing to buffer" and "I lost rows" stay
  // distinguishable at commit.
  void mark_delta_unknown() {
    std::lock_guard<std::mutex> guard(trigger_buffer_mutex_);
    capture_.trigger_fed = true;
    capture_.unknown_writes = true;
  }
  // ---- end trigger-source surface --------------------------------------

  // dbsp_track added this key on this connection. DBSP's tracked-table set is
  // NOT transactional, so a track issued inside a transaction that rolls back
  // used to survive it: dbsp_tables() went on listing a table that never
  // committed, and the trigger sweep would install bodies on whatever table
  // later took that name. TransactionRollback drops the intent instead.
  void note_table_tracked(const std::string &key) {
    tracked_in_txn_.push_back(key);
  }

  // The CDC core could not establish some table's baseline right now, because
  // reading committed state on an internal connection while this transaction
  // is open would miss its uncommitted rows (CDCManager::seed_baseline). The
  // baseline is EMPTY until something scans, and any view built over it is
  // wrong until then, so this flag is STICKY: it is not cleared at a
  // transaction boundary, only when a reconcile actually runs.
  //
  // TransactionCommit under auto-sync widens itself to a full scan-and-diff
  // for it; TransactionRollback, which has no commit to reconcile, asks for a
  // rebuild of every view from committed storage. A commit with auto-sync OFF
  // reconciles nothing, so it leaves the flag alone.
  void note_unseeded_baseline() { unseeded_baseline_ = true; }

  void QueryBegin(duckdb::ClientContext &context) override {
    if (internal_query_depth > 0) {
      return;
    }
    maybe_run_recovery(context);
    auto &manager = get_cdc_manager(context);
    // Trigger source: bring the catalog's DBSP triggers in line with the
    // tracked-table set. This is the only sweep point, so it covers every way
    // a table becomes tracked (dbsp_track, view-source auto-tracking, a
    // checkpoint restore) without enumerating entry points. Cost in the steady
    // state is one shared-lock size comparison per statement.
    //
    // The install commits on its own connection, i.e. AFTER this statement's
    // transaction took its catalog snapshot — so the triggers cannot fire for
    // THIS transaction. capture_.triggers_installed makes its commit
    // reconcile by scan; every transaction after it sees the triggers.
    //
    // Both outcomes that are not UNCHANGED mean the same thing to this
    // transaction: some tracked table's trigger bodies do not match its
    // columns right now, so nothing they buffer can be trusted and the
    // commit must reconcile by scan.
    //
    // CHANGED — the bodies were just regenerated on an internal connection,
    // which commits AFTER this transaction took its catalog snapshot, so
    // this transaction still runs against the old ones.
    // DEFERRED — the user holds a transaction open, so the DDL could not run
    // at all (an internal connection cannot see their uncommitted catalog
    // changes; trying anyway made their own COMMIT fail and wedged the
    // connection). The reconcile waits for the first statement after their
    // transaction ends, and `recheck` stays armed until it succeeds.
    //
    // DETACH is the one statement the sweep must not run before: the gate
    // reads catalog versions, which joins those catalogs to this transaction,
    // and DETACH refuses to run against a catalog the transaction has touched.
    if (!statement_detaches(context.GetCurrentQuery()) &&
        install_pending_triggers(context, manager) !=
            ReconcileResult::UNCHANGED) {
      capture_.triggers_installed = true;
    }
    // D3c: an out-of-band change invalidated a lazily-restored baseline —
    // reconciliation is impossible incrementally, so views rebuild from
    // committed storage at the next statement boundary (here). Runs even
    // with auto-sync off (the notify path can schedule it too). D-lazy
    // reuses this same flag/sweep for a corrupt lazy-restored VIEW stash
    // (realize_pending_view_locked, dbsp_cdc.hpp) — rare either way, same
    // escape hatch.
    if (manager.rebuild_pending()) {
      try {
        manager.rebuild_all_views(context);
      } catch (const std::exception &e) {
        std::cerr << "DBSP: view rebuild failed: " << e.what() << "\n";
      }
    }
    const bool auto_sync = manager.is_auto_sync_enabled();
    if (!auto_sync && !manager.has_deferred()) {
      stmt_ = {};
      return; // no auto-sync: don't pay the parse on every statement
    }
    // Classify here: GetCurrentQuery() is already cleared by QueryEnd, and
    // for autocommit statements the commit hook fires mid-query — before
    // QueryEnd — so the commit-time sync scoping needs the classification
    // of the in-flight statement (stmt_), not just per-txn accumulation.
    // With auto-sync OFF this parse runs only while baselines are deferred
    // (D3c) — the write check below is the sole consumer.
    stmt_ = classify(context.GetCurrentQuery());
    if (std::getenv("DBSP_DEBUG_SYNC")) {
      last_query_dbg_ = context.GetCurrentQuery();
    }
    // D3c: a write is about to execute. Deferred (lazy-restored) baselines
    // must materialize from PRE-write storage: this is the only moment the
    // scan is guaranteed to equal the restore-time content exactly. (The
    // notify path can materialize later with pending-delta subtraction,
    // but only the first fragment of a compound edit is known then —
    // materializing here keeps SQL-write-then-notify hosts exact.)
    if ((stmt_.kind == StmtClass::INSERT_OK ||
         stmt_.kind == StmtClass::WRITE_KNOWN ||
         stmt_.kind == StmtClass::WRITE_UNKNOWN) &&
        manager.has_deferred()) {
      manager.materialize_all_deferred(context);
      if (manager.rebuild_pending()) {
        try {
          manager.rebuild_all_views(context);
        } catch (const std::exception &e) {
          std::cerr << "DBSP: view rebuild failed: " << e.what() << "\n";
        }
      }
    }
    if (!auto_sync) {
      stmt_ = {}; // hooks are inert without auto-sync
      return;
    }
    // Autocommit statements fold NOW: their commit hook fires mid-
    // statement when the catalog view is already unusable (resolve
    // fails), and their QueryEnd runs post-commit — this is the only
    // hook where scoping resolution works. Explicit-txn statements fold
    // at QueryEnd instead.
    if (context.transaction.HasActiveTransaction() &&
        context.transaction.IsAutoCommit()) {
      fold_into_txn(context, stmt_);
    }
  }

  void TransactionBegin(duckdb::MetaTransaction &transaction,
                        duckdb::ClientContext &context) override {
    if (internal_query_depth > 0) {
      return;
    }
    capture_ = {};
    tracked_in_txn_.clear();
    // unseeded_baseline_ is NOT cleared here: an empty baseline outlives the
    // transaction that left it that way, and only a reconcile repairs it.
  }

  void QueryEnd(duckdb::ClientContext &context,
                duckdb::optional_ptr<duckdb::ErrorData> error) override {
    if (internal_query_depth > 0) {
      return;
    }
    auto &manager = get_cdc_manager(context);
    if (!manager.is_auto_sync_enabled()) {
      return;
    }
    if (error && error->HasError()) {
      return; // failed statement changed nothing
    }
    // Autocommit: the mid-statement commit hook already consumed the
    // statement and reset capture_ — folding here would re-count it against
    // a finished transaction whose catalog view is gone (resolve fails,
    // touched stays empty) and poison the NEXT statement's commit into a
    // read-only skip.
    if (!context.transaction.HasActiveTransaction() ||
        context.transaction.IsAutoCommit()) {
      return;
    }

    try {
      fold_into_txn(context, stmt_);
    } catch (const std::exception &) {
      // Scoping is an optimization only: on any surprise, widen this
      // transaction's commit to the full scan-and-diff.
      capture_.unknown_writes = true;
    } catch (...) {
      capture_.unknown_writes = true;
    }
  }

  void TransactionCommit(duckdb::MetaTransaction &transaction,
                         duckdb::ClientContext &context) override {
    // Skip commits from DBSP's own internal helper connections - recursing
    // into CDCManager here deadlocks (issuing thread may hold struct_mutex_)
    if (internal_query_depth > 0) {
      return;
    }

    tracked_in_txn_.clear(); // committed: the tracking intent stands

    auto &manager = get_cdc_manager(context);
    if (!manager.is_auto_sync_enabled()) {
      // unseeded_baseline_ is deliberately LEFT SET: this commit reconciles
      // nothing, so the baseline is still empty. Clearing it here made the
      // deferred seeding vanish — the view stood on an empty baseline for the
      // rest of the connection's life (measured: 4.0 against SQL 17.0 after
      // auto-sync was turned back on). An explicit dbsp_sync(), or the first
      // commit once auto-sync is back on, is what repairs it.
      capture_ = {};
      return;
    }

    // A baseline left unseeded (seed_baseline could not scan inside an open
    // user transaction) is reconciled HERE, by widening this commit to a full
    // scan-and-diff. It has to be a WIDENING and not a note in
    // capture_.touched: the trigger-fed fast path below applies its buffered
    // deltas and returns without ever reading `touched`, so a trigger-fed
    // commit discarded the reconcile silently and the view read 3.0 against
    // SQL 13.0 forever. unknown_writes is the one flag BOTH commit branches
    // honour. saw_statements keeps the "nothing fed, no statement seen" early
    // return below from skipping the sync.
    //
    // The scan is unconditional once the flag is set, so an explicit
    // dbsp_sync() that already repaired the baseline costs one extra
    // no-difference scan here. That is the price of not plumbing sync
    // observation back into the connection state; it is paid once.
    //
    // The flag is NOT cleared here. A reconcile scan can fail — the source
    // was dropped, a deferred materialization threw — and CDCManager returns
    // that as `false` rather than by throwing. Clearing the debt before the
    // scan that pays it meant one failed scan left the baseline empty for the
    // life of the connection, with nothing owed and nothing said. settle()
    // below clears it only on a sync that reports success.
    if (unseeded_baseline_) {
      capture_.unknown_writes = true;
      capture_.saw_statements = true;
    }
    // Only a FULL reconcile settles the debt: the flag names no table, so a
    // scoped sync_tables cannot prove it covered the unseeded one. In
    // practice a scoped sync never runs while the debt stands, because the
    // widening above forces unknown_writes and every branch below then takes
    // sync_all.
    auto settle = [this](bool sync_ok) {
      if (sync_ok) {
        unseeded_baseline_ = false;
      }
    };

    // Auto-persist checkpoint interval (dbsp_autopersist_interval): fires a
    // piggybacked save_checkpoint() once enough commits have accumulated
    // (counted by propagate_changes), regardless of which of this
    // function's several sync paths/early returns below applied. struct_
    // mutex_ is never held by this thread once TransactionCommit returns,
    // so it's safe to run save_checkpoint's own locking at that point.
    struct CheckpointGuard {
      CDCManager &m;
      duckdb::ClientContext &ctx;
      ~CheckpointGuard() {
        m.maybe_save_checkpoint(ctx);
        // Trigger source housekeeping: the one row each trigger firing writes
        // into its sink is write-only bookkeeping; drain it periodically so a
        // long-lived process cannot grow the sink without bound. An atomic
        // load below the threshold, and nothing else.
        maybe_drain_trigger_sinks(ctx);
      }
    } checkpoint_guard{manager, context};

    try {
      // Trigger-fed fast path: the bodies reported this transaction's exact
      // per-table images — facts need no guards. unknown_writes still forces
      // a scan: a firing that could not reach the buffer, or a conversion
      // that failed, leaves an incomplete picture.
      if (capture_.trigger_fed) {
        // triggers_installed means the trigger bodies did not match their
        // tables for the whole of this transaction: either a table became
        // tracked under it, or a schema moved and regeneration was deferred.
        // Whatever they buffered was produced by a body of the wrong shape, so
        // it is DISCARDED rather than applied and then repaired — a
        // wrong-width row applied to a baseline is a mess a later scan has to
        // undo.
        if (capture_.triggers_installed) {
          capture_ = {};
          settle(manager.sync_all(context, &transaction));
          return;
        }
        const bool unknown = capture_.unknown_writes;
        auto deltas = std::move(capture_.trigger_deltas);
        capture_ = {};
        // ALL tables in ONE propagation pass — per-table applies kept only
        // the last table's downstream deltas (single-generation buffers)
        // and missed join both-shared corrections. Tables the manager
        // could not serve (untracked-by-now or deferred-baseline rebuild
        // scheduled) are reconciled by scan below.
        std::vector<std::string> failed =
            manager.apply_captured_deltas(deltas, &context);
        if (unknown) {
          settle(manager.sync_all(context, &transaction));
        } else if (!failed.empty()) {
          manager.sync_tables(context, failed,
                              manager.parallel_sync_enabled() &&
                                  failed.size() > 1,
                              &transaction);
        }
        return;
      }

      // H1: scope the fallback sync to the tables this transaction wrote.
      // Autocommit commits fire mid-statement, before QueryEnd folded the
      // in-flight classification — fold it now.
      if (!capture_.saw_statements && stmt_.kind != StmtClass::NONE) {
        fold_into_txn(context, stmt_);
      }
      const bool know_all_writes =
          capture_.saw_statements && !capture_.unknown_writes;
      const bool saw_statements = capture_.saw_statements;
      std::vector<std::string> touched(capture_.touched.begin(),
                                       capture_.touched.end());
      capture_ = {};

      if (know_all_writes && touched.empty()) {
        return; // read-only commit: nothing can have changed
      }
      // The triggers report EVERY write to a tracked table (the trigger_fed
      // branch above), so a commit that reaches here with nothing fed and NO
      // statement seen provably wrote nothing to a tracked table — fresh
      // connections fire setup/teardown commits with exactly this signature,
      // and the pre-H1 sync_all safety net turned each one into a full
      // scan-diff of every tracked table. DDL cannot hide here (it is
      // statement-shaped and folds as WRITE_UNKNOWN, so saw_statements is true
      // and the sync below still runs). Gated on trigger_source_active(),
      // which flips only once a trigger body has actually DELIVERED: until
      // then the pessimistic net stands.
      if (trigger_source_active() && !saw_statements &&
          stmt_.kind == StmtClass::NONE) {
        return;
      }
      if (std::getenv("DBSP_DEBUG_SYNC")) {
        std::cerr << "[dbsp] commit fallback sync: know_all="
                  << know_all_writes << " touched=" << touched.size()
                  << " stmt_kind=" << static_cast<int>(stmt_.kind)
                  << " thread=" << std::this_thread::get_id() << " ctx="
                  << &context << " tracked=" << manager.tracked_table_total()
                  << " last_q='" << last_query_dbg_.substr(0, 60) << "'\n";
      }
      // The transaction has already committed successfully at this point.
      // We can safely read the post-commit state and pass the transaction
      // for proper catalog access.
      if (know_all_writes) {
        manager.sync_tables(context, touched,
                            manager.parallel_sync_enabled() &&
                                touched.size() > 1,
                            &transaction);
      } else {
        // Writes we could not attribute (multi-statement, unparseable SQL):
        // scan everything, as before H1
        settle(manager.sync_all(context, &transaction));
      }
    } catch (const std::exception &ex) {
      std::cerr << "DBSP Auto-CDC error: " << ex.what() << "\n";
    } catch (...) {
      std::cerr << "DBSP Auto-CDC unknown error\n";
    }
  }

  void TransactionRollback(duckdb::MetaTransaction &transaction,
                           duckdb::ClientContext &context) override {
    if (internal_query_depth > 0) {
      return;
    }
    capture_ = {}; // rolled back: buffered rows never happened
    // An unseeded baseline has no commit to reconcile it now. Rebuild instead:
    // rebuild_all_views refreshes EVERY tracked baseline from committed
    // storage before it replays the views, so it is a stronger repair than the
    // commit's scan-and-diff and the flag is cleared here too.
    if (unseeded_baseline_) {
      unseeded_baseline_ = false;
      get_cdc_manager(context).request_rebuild();
    }
    // The tracked-table set is process state, not transactional state — so a
    // dbsp_track inside this transaction has to be undone by hand. Without
    // this, a rolled-back CREATE TABLE u + dbsp_track('u') left u tracked
    // forever against a table that does not exist.
    if (!tracked_in_txn_.empty()) {
      auto &manager = get_cdc_manager(context);
      for (const auto &key : tracked_in_txn_) {
        manager.untrack_table(key);
      }
      tracked_in_txn_.clear();
    }
  }

  // One-time crash recovery, moved here from OnConnectionOpened: that
  // callback runs under ConnectionManager::connections_lock, and recovery
  // opens internal Connections whose constructors re-enter AddConnection —
  // a self-deadlock. QueryBegin runs without that lock. Guarded so
  // recovery's own internal connections (internal_query_depth > 0) and
  // re-entrant queries never recurse.
  static void maybe_run_recovery(duckdb::ClientContext &context) {
    // Once per DATABASE, not once per process. A process-wide latch meant
    // only the first database opened ever ran recovery, so every later one
    // got no crash marker and registered no session — leaving them
    // unprotected and making the first close drop the shared lock.
    if (!dbsp_claim_recovery(context.db.get())) {
      return;
    }
    auto &recovery_manager = get_recovery_manager();
    std::string db_path;
    try {
      auto &db_manager = duckdb::DatabaseManager::Get(context);
      // The default catalog's NAME is not its path ("m" for m.duckdb) and
      // the old lookup by DEFAULT_SCHEMA ("main") never matched anything —
      // which parked recovery markers in the process CWD for every mode.
      auto default_db = db_manager.GetDatabase(
          context, duckdb::DatabaseManager::GetDefaultDatabase(context));
      if (default_db) {
        auto &storage = default_db->GetStorageManager();
        if (!storage.InMemory()) {
          db_path = storage.GetDBPath();
        }
      }
    } catch (...) {
      // no durable default catalog: markers stay disabled
    }
    recovery_manager.recover_from_crash(context, db_path);
  }

private:
  struct TxnCapture {
    // H1 sync scoping: which tables this transaction wrote, and whether we
    // saw/classified every statement. Writes we could not attribute
    // (unparseable, multi-statement, unknown statement kinds) force a full
    // sync; so does a transaction with zero seen statements that the trigger
    // bodies have not vouched for.
    bool saw_statements = false;
    bool unknown_writes = false;
    std::unordered_set<std::string> touched;
    // Exact per-table images the trigger bodies reported for this transaction
    // (buffer_trigger_delta); TransactionCommit applies them guard-free.
    std::unordered_map<std::string, DuckDBZSet> trigger_deltas;
    bool trigger_fed = false;
    // Trigger source: this transaction had DBSP triggers installed under it
    // (QueryBegin sweep). The install commits on its own connection AFTER
    // this transaction took its catalog snapshot, so the triggers cannot fire
    // for it — its picture is incomplete and commit must reconcile by scan.
    bool triggers_installed = false;
  };
  TxnCapture capture_;
  // Keys dbsp_track ADDED under the in-flight transaction (note_table_tracked)
  std::vector<std::string> tracked_in_txn_;
  // See note_unseeded_baseline().
  bool unseeded_baseline_ = false;
  // Trigger bodies run on execution threads, and an aggregate over a large
  // transition table may be parallel, so more than one thread can be inside
  // buffer_trigger_delta at once. Guards trigger_deltas only.
  std::mutex trigger_buffer_mutex_;

  // Classification of one SQL statement (computed at QueryBegin)
  struct StmtClass {
    enum Kind {
      NONE,        // empty / not seen
      READ,        // changes nothing
      INSERT_OK,   // plain INSERT with a known target table
      WRITE_KNOWN, // write with a known target table (DELETE/UPDATE/upsert)
      WRITE_UNKNOWN // anything else that might write anywhere
    };
    Kind kind = NONE;
    std::string table;   // INSERT_OK / WRITE_KNOWN target
  };
  StmtClass stmt_;
  std::string last_query_dbg_; // DBSP_DEBUG_SYNC only: last classified query

  static std::string base_table_name(const duckdb::TableRef *ref) {
    if (ref && ref->type == duckdb::TableReferenceType::BASE_TABLE) {
      auto &base = ref->Cast<duckdb::BaseTableRef>();
      // DuckDB 2.0: BaseTableRef holds one QualifiedName behind accessors
      const auto &qn = base.GetQualifiedName();
      return dotted_ref(qn.Catalog().GetIdentifierName(),
                        qn.Schema().GetIdentifierName(),
                        qn.Name().GetIdentifierName());
    }
    return {};
  }

  // Textual dotted reference from parsed statement parts (either or both
  // qualifiers may be absent). Canonicalization happens at fold time via
  // resolve_table_entry — parse-time text is never used as a key.
  static std::string dotted_ref(const std::string &catalog,
                                const std::string &schema,
                                const std::string &table) {
    std::string out;
    if (!catalog.empty()) {
      out += catalog + ".";
    }
    if (!schema.empty()) {
      out += schema + ".";
    }
    return out + table;
  }

  StmtClass classify(const std::string &query) {
    StmtClass out;
    if (query.empty()) {
      return out;
    }
    // dbsp's own DDL: CREATE MATERIALIZED VIEW only READS tracked tables to
    // populate the new view — it never writes one. The custom syntax fails
    // core parsing, so without this carve-out it fell into "unparseable:
    // assume the worst" → WRITE_UNKNOWN → sync_all, a full scan-diff of
    // EVERY tracked table on every view creation — O(views × rows) DAG
    // builds (the residual after the shadow-table fix).
    {
      const auto first = query.find_first_not_of(" \t\r\n");
      if (first != std::string::npos) {
        const auto head = duckdb::StringUtil::Lower(query.substr(first, 24));
        if (head == "create materialized view") {
          out.kind = StmtClass::READ;
          return out;
        }
        // Plain reads skip the full parse: with auto-sync on, classify runs
        // on EVERY statement, and constructing a Parser + ParseQuery for
        // each SELECT taxed read-heavy workloads. Only unambiguous read
        // heads short-circuit — "with" is excluded (data-modifying CTEs),
        // "pragma" can write settings; everything else still parses.
        const auto head_kw = [&head](const char *kw, size_t n) {
          if (head.rfind(kw, 0) != 0) {
            return false;
          }
          return head.size() == n ||
                 !(std::isalnum(static_cast<unsigned char>(head[n])) ||
                   head[n] == '_');
        };
        if (head_kw("select", 6) || head_kw("explain", 7)) {
          out.kind = StmtClass::READ;
          return out;
        }
      }
    }
    duckdb::Parser parser;
    try {
      parser.ParseQuery(query);
    } catch (...) {
      out.kind = StmtClass::WRITE_UNKNOWN; // unparseable: assume the worst
      return out;
    }
    if (parser.statements.size() != 1) {
      out.kind = parser.statements.empty() ? StmtClass::NONE
                                           : StmtClass::WRITE_UNKNOWN;
      return out;
    }
    auto &stmt = *parser.statements[0];
    switch (stmt.type) {
    case duckdb::StatementType::SELECT_STATEMENT:
    case duckdb::StatementType::EXPLAIN_STATEMENT:
    case duckdb::StatementType::PREPARE_STATEMENT:
    case duckdb::StatementType::TRANSACTION_STATEMENT:
      out.kind = StmtClass::READ;
      return out;
    case duckdb::StatementType::INSERT_STATEMENT: {
      auto &insert = stmt.Cast<duckdb::InsertStatement>();
      // DuckDB 2.0: the DML payload moved onto a QueryNode (InsertQueryNode)
      const auto &qn = insert.node->qualified_name;
      out.table = dotted_ref(qn.Catalog().GetIdentifierName(),
                             qn.Schema().GetIdentifierName(),
                             qn.Name().GetIdentifierName());
      out.kind = insert.node->on_conflict_info ? StmtClass::WRITE_KNOWN
                                               : StmtClass::INSERT_OK;
      return out;
    }
    case duckdb::StatementType::DELETE_STATEMENT: {
      auto &del = stmt.Cast<duckdb::DeleteStatement>();
      out.table = base_table_name(del.node->table.get());
      out.kind =
          out.table.empty() ? StmtClass::WRITE_UNKNOWN : StmtClass::WRITE_KNOWN;
      return out;
    }
    case duckdb::StatementType::UPDATE_STATEMENT: {
      auto &upd = stmt.Cast<duckdb::UpdateStatement>();
      out.table = base_table_name(upd.node->table.get());
      out.kind =
          out.table.empty() ? StmtClass::WRITE_UNKNOWN : StmtClass::WRITE_KNOWN;
      return out;
    }
    default:
      out.kind = StmtClass::WRITE_UNKNOWN;
      return out;
    }
  }

  // Merge one statement's classification into the transaction's sync scope
  void fold_into_txn(duckdb::ClientContext &context, const StmtClass &c) {
    if (c.kind == StmtClass::NONE) {
      return;
    }
    capture_.saw_statements = true;
    switch (c.kind) {
    case StmtClass::READ:
      break;
    case StmtClass::INSERT_OK:
    case StmtClass::WRITE_KNOWN: {
      // Canonicalize the parsed reference; a target that does not resolve
      // cannot be a tracked table (tracked keys always resolve), so skip.
      auto entry = resolve_table_entry(context, c.table);
      if (entry &&
          get_cdc_manager(context).is_table_tracked(
              canonical_table_key(*entry))) {
        capture_.touched.insert(canonical_table_key(*entry));
      }
      break;
    }
    default:
      capture_.unknown_writes = true;
      break;
    }
  }
};

} // namespace dbsp_native
