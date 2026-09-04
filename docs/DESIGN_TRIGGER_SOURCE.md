# Trigger-fed delta source

Status: the ONLY delta source (2026-09-03). No switch, no fallback mechanism —
the scan-and-diff reconcile is the safety net, not a second source.

## The one-sentence version

DuckDB 2.0 grew statement-level `AFTER` triggers with transition tables, and
trigger expansion happens in the **binder** — so a trigger sees every write the
binder sees, which in 2.0 includes the Appender. That makes it possible to get
the exact per-statement old/new row images **on a stock engine**, with no
patched build, no `patches/` directory, and no CI story that cannot be
reproduced from a public wheel.

## How it works

`dbsp_track(t)` (or any other route that ends with `t` tracked) causes three
triggers to be generated on `t`:

```sql
CREATE OR REPLACE TRIGGER dbsp_trg_t_ins AFTER INSERT ON "cat"."sch"."t"
  REFERENCING NEW TABLE AS dbsp_new FOR EACH STATEMENT
  INSERT INTO "cat"."sch"."dbsp_trigger_sink"
    SELECT max(dbsp_trigger_ingest('cat.sch.t', 1, "c1", "c2", ...)) FROM dbsp_new;

CREATE OR REPLACE TRIGGER dbsp_trg_t_del AFTER DELETE ON "cat"."sch"."t"
  REFERENCING OLD TABLE AS dbsp_old FOR EACH STATEMENT
  INSERT INTO "cat"."sch"."dbsp_trigger_sink"
    SELECT max(dbsp_trigger_ingest('cat.sch.t', -1, "c1", ...)) FROM dbsp_old;

CREATE OR REPLACE TRIGGER dbsp_trg_t_upd AFTER UPDATE ON "cat"."sch"."t"
  REFERENCING OLD TABLE AS dbsp_old NEW TABLE AS dbsp_new FOR EACH STATEMENT
  INSERT INTO "cat"."sch"."dbsp_trigger_sink" SELECT max(v) FROM (
      SELECT dbsp_trigger_ingest('cat.sch.t', -1, "c1", ...) AS v FROM dbsp_old
      UNION ALL
      SELECT dbsp_trigger_ingest('cat.sch.t',  1, "c1", ...) AS v FROM dbsp_new);
```

`dbsp_trigger_ingest(key, weight, cols...)` is a vectorised extension
scalar (`src/dbsp_extension.cpp`, `TriggerIngestScalar`). It is `VOLATILE` (so
it is never folded or cached) with `SPECIAL_HANDLING` null handling (row images
are full of NULLs; the default would have skipped them). It converts its chunk
of row images into a signed Z-set — reading typed vector data directly, exactly
like `engine_cdc_to_zset` — and writes it into
`DBSPContextState::buffer_trigger_delta`, the committing connection's
per-transaction buffer.

Everything downstream is therefore unchanged: `TransactionCommit` applies the
buffer through `apply_captured_deltas` in one pass over all tables, and
`TransactionRollback` clears it. `dbsp_cdc.hpp` was not touched.

### Why the aggregate and the sink

A trigger body must be a DML statement — `CreateTriggerInfo::trigger_action` is
a `QueryNode`, there is no procedural body and no C++ trigger API anywhere in
the engine (`TriggerCallback` does not exist). So the ingest call has to ride
inside an `INSERT`. `max(...)` over the transition table forces per-row
evaluation while collapsing the whole firing to **one** sink row.

The sink lives in the **tracked table's own catalog**, not a fixed one: DuckDB
refuses to write two catalogs in a single transaction, so a sink anywhere else
would break every tracked write. Nothing ever reads it; it is drained
(`DELETE FROM`) every 50,000 firings from the commit hook's housekeeping guard.

Considered and rejected: a staging table holding full row images that the
extension reads at commit. It collides with a law this fork already recorded
(*catalog lookups inside `DuckTransaction::Commit` are unsafe*), and it
would materialise every changed row into real storage and WAL and read it back.

### Installing the triggers

One sweep, in `DBSPContextState::QueryBegin`. That covers every way a table
becomes tracked — `dbsp_track`, view-source auto-tracking inside `create_view`,
a checkpoint restore — without enumerating entry points, which is the kind of
list that silently goes stale. In the steady state it costs one shared-lock
size comparison per statement.

The sweep runs its DDL on its own internal connection, so it commits **after**
the current statement's transaction took its catalog snapshot. That transaction
therefore cannot see the new triggers. This is handled, not ignored:
`capture_.triggers_installed` makes that one commit reconcile by scan
(`unknown` in the engine-fed branch), and every transaction that begins
afterwards is served by triggers. Cost: one scan-and-diff, once, per newly
tracked table.

`CREATE OR REPLACE` makes re-tracking free of "trigger already exists"
failures. The install record is per database and holds a **weak reference** to
its `DatabaseInstance`: keying it on the raw address alone was wrong in exactly
the way this fork has recorded before — DuckDB reuses freed addresses, and
consecutive test harnesses in one process inherited a stale "already installed"
list and ran with no triggers at all. Two tests failed on that and nothing
else; it is a real bug class, not a theoretical one.

**The catalog is the record, not this process's memory.** Trigger names carry a
hash of the table key AND the column fingerprint
(`dbsp_trg_<table>_<hash>_<op>`), so a process that finds the three names in
`duckdb_triggers()` knows the installed bodies match the table's current
columns without re-issuing any DDL. This matters because re-issuing is not
free: the new bodies commit on an internal connection AFTER the current
statement took its catalog snapshot, so the sweep has to mark that transaction
untrusted and its commit reconciles by scanning every tracked table. Measured
before the names carried the fingerprint: the FIRST WRITE AFTER EVERY REOPEN
paid that full scan, and it also cost the delta-append sidecar save
(`test/python/test_delta_append_sidecars.py` went red — a dirty save rewrote
the whole base digest index instead of appending an O(changed-rows) delta).

A fingerprint change (only `ADD COLUMN` reaches this — every other schema
change is refused on a triggered table) produces different names, so the
install also DROPS by name every `dbsp_trg_*` trigger on that table that is not
one of the three it is about to create. `CREATE OR REPLACE` alone would leave
the old-fingerprint bodies in place, delivering rows of the wrong width
alongside the new ones.

The presence check reads `duckdb_triggers()` on an internal connection. That is
a plain READ, so unlike the DDL it is safe to run while the user holds a
transaction open: it takes its own snapshot rather than trying to see their
uncommitted catalog changes. It is paid only when this process's install record
disagrees with the tracked set — once per database in the steady state.
(`Catalog::GetEntry` is not an option: it returns nothing for
`CatalogType::TRIGGER_ENTRY` even when the trigger is right there in
`duckdb_triggers()`, measured on `v2.0.0-alpha39998`.)

### No double counting

Each firing must deliver its rows exactly once. Pinned by
`trigger source: one insert counts exactly once`, which asserts a **sum**, not
a row count: a row delivered twice doubles the sum while leaving the row count
untouched.

`trigger_source_flag()` is the proof of life. It flips only on a **delivered**
ingest, never at install time, and the commit path's cheap "this transaction
saw no statements, so it wrote nothing" early return is gated on it. Triggers
that exist but never fire therefore cost scans, not silent staleness.

## Write-path coverage

Measured on this build, not inferred. Every row is pinned by
`test/unit/test_trigger_source.cpp`.

| Write path | Covered? | Evidence |
|---|---|---|
| `INSERT ... VALUES` (multi-row) | Yes | `insert/update/delete keep the view current` |
| `INSERT ... SELECT` | Yes | C++ suite |
| `UPDATE` (both images) | Yes | `update chain collapses to first-old / last-new` |
| `DELETE` | Yes | `insert/update/delete keep the view current` |
| Multi-statement single transaction | Yes | `multi-statement single transaction` (4 sections) |
| Multi-table single transaction | Yes | `multi-table one-transaction commit` |
| `BEGIN ... ROLLBACK` | Yes | `explicit rollback discards everything` |
| C++ `Appender` | Yes | `the C++ Appender updates the view` (3 rows, sum 61.0) |
| `COPY ... FROM` (CSV) | Yes | `COPY FROM a CSV updates the view` (sum 111.0) |
| `TRUNCATE` | Yes, as a full delete | C++ suite |
| NULL-bearing rows | Yes | `NULLs survive the round trip` |
| Multi-chunk DML under `threads=8` | Yes | C++ suite (20,001 rows, INSERT/UPDATE/DELETE) |
| A user's own trigger on the same table | Yes, coexists | C++ suite |
| `ALTER TABLE ... ADD COLUMN` then write | Yes, bodies regenerate | C++ suite |
| `DROP TABLE` + recreate + re-track | Yes, triggers reinstalled | C++ suite |
| `INSERT ... ON CONFLICT DO NOTHING` | Yes | C++ suite |
| **`MERGE INTO` a tracked table** | **NO — hard engine error** | `MERGE INTO on a tracked table is rejected` |
| **`INSERT ... ON CONFLICT DO UPDATE`** | **NO — hard engine error** | C++ suite |
| **`INSERT OR REPLACE`** | **NO — hard engine error** | C++ suite |
| **`ALTER TABLE` other than ADD COLUMN** | **NO — hard engine error** | C++ suite (RENAME COLUMN pinned) |
| `InternalAppender` (engine-internal) | No | `appender.cpp:747-750` calls `LocalAppend`, bypassing the binder |

### What tracking a table COSTS it

This is the price of the source, and it is bigger than one statement. Every
item below is an engine behaviour, measured on this build and pinned by a test;
all of them work normally in default and capture mode.

| On a tracked (triggered) table | Engine response |
|---|---|
| `MERGE INTO <t> ...` | `Not implemented Error: MERGE INTO is not supported on tables with triggers` |
| `INSERT ... ON CONFLICT DO UPDATE` | `Not implemented Error: ON CONFLICT DO UPDATE is not yet supported with REFERENCING NEW TABLE AS triggers` |
| `INSERT OR REPLACE` (same path) | same error |
| `ALTER TABLE ... DROP COLUMN` | `Dependency Error: Cannot alter entry "t" because there are entries that depend on it.` |
| `ALTER TABLE ... RENAME COLUMN` | same dependency error |
| `ALTER TABLE ... ALTER COLUMN ... TYPE` | same dependency error |
| `ALTER TABLE ... RENAME TO` | same dependency error |
| `ALTER TABLE ... ADD COLUMN` | **allowed**, and handled — the sweep notices via the catalog version and regenerates the bodies; done inside a transaction, that transaction's commit reconciles by scan and the regeneration happens after it ends (below) |
| `INSERT ... ON CONFLICT DO NOTHING` | allowed |
| `DROP TABLE` | allowed; takes the triggers with it, and a recreate + re-track reinstalls them |

Sources: `duckdb/src/planner/binder/statement/bind_merge_into.cpp:226-233` for
MERGE; the other two messages are the engine's own, reproduced in the shell on
this build.

So this source does not merely learn what changed — it **removes upserts and
most schema changes from every table NumPad tracks**. Neither of the two
sources it replaced cost anything like that. An earlier draft of this document
called MERGE "the one real price"; that was wrong, and the full list is the
thing to weigh. The owner accepted it knowingly on 2026-09-03.

For NumPad specifically: the single `MERGE INTO` site is
`api/integration/transforms/service.py:371`, targeting `land.<relation>` in the
data-integration landing schema, and nothing tracks `land.*`. So none of this
bites today — but it is a standing constraint on every tracked table.

### Keeping the bodies in step with the table

A generated body encodes the column list literally, so anything that moves a
tracked table's schema makes every installed body stale. Most such statements
the engine refuses outright (above); `ADD COLUMN` does not, and `DROP TABLE`
silently takes the triggers away.

Neither moves the tracked-table **count**, which is the cheapest steady-state
check available — so a count-only sweep missed both, and both produced wrong
answers with no scan to catch them: a view read 5.0 where SQL read 12.0 after an
`ADD COLUMN`, and a dropped-and-recreated table stayed tracked but permanently
triggerless.

**What tells the sweep to look.** `Catalog::GetCatalogVersion(context)` — the
same signal prepared statements use to invalidate themselves. It moves on any
committed catalog change, and while a transaction holds uncommitted catalog
changes of its own it returns a value above `TRANSACTION_START`. That second
property is the load-bearing one: it makes a transaction's own in-flight
`ALTER` visible to the sweep. The version is cached per catalog holding tracked
tables and compared at every `QueryBegin`, alongside the tracked-table count and
an explicit `recheck` flag. An earlier version of this sweep sniffed the leading
keyword of the statement text instead; that could not see DDL arriving any other
way, and could not see an open transaction's own changes at all.

**What it compares.** A **column fingerprint** — name and type of every column,
in order — taken from the LIVE catalog through `resolve_table_entry`, which is a
plain catalog lookup on the caller's own context and runs no SQL. Comparing the
manager's cached schema instead would be comparing the bodies' input with
itself. When a fingerprint moves, or when `duckdb_triggers()` shows a table has
lost its triggers, the bodies are regenerated with `CREATE OR REPLACE`.

**Where the DDL runs, and where it must not.** Regeneration runs on an internal
connection, which by construction cannot see another transaction's uncommitted
catalog changes. Running it while the user holds a transaction open therefore
fails by construction — measured, it threw
`Binder Error: Referenced column "note" not found` **out of the user's own
COMMIT**, and a `CREATE TABLE` + `dbsp_track` in one transaction wedged the
connection so completely that its `ROLLBACK` and even `SELECT 1` threw
`Catalog Error: Table with name u does not exist!` until it was closed.

So the sweep **defers**: while a user transaction is open it does no DDL at all.
It marks that transaction's delta untrusted — the commit discards whatever the
stale bodies buffered and reconciles by scan instead — leaves `recheck` armed,
and regenerates at the first statement after the transaction ends. The same
applies to a `dbsp_track` issued inside a transaction: the triggers appear once
it commits, and if it rolls back the table is simply recorded as absent.

`recheck` is cleared **only** on a reconcile that succeeded. Clearing it up
front, as the first version did, meant any throw disarmed the one thing that
would have retried.

### Other standing costs

- The triggers and the sink are **user-visible catalog objects** on the user's
  tables and schemas: they show in `duckdb_triggers()` / `duckdb_tables()`, are
  WAL-logged, and appear in `EXPORT DATABASE`.
- A bulk Appender fires once per flush chunk (~2048 rows), not once per
  transaction. Z-set additivity makes that correct, but it is many firings.
- `dbsp_untrack` does not exist as an extension function, so the sweep's
  drop-DDL branch (triggers removed when a table stops being tracked) is
  **unreachable today** and therefore untested. It runs only if such an entry
  point is added.
- Tracking a table in a **read-only** attached catalog cannot work — the sink
  cannot be created. The install throws rather than going quietly stale.
- The `internal_query_depth` self-ingest guard is thread-local, so it only
  covers work executed on the issuing thread. That is enough because DBSP never
  writes a tracked user table from an internal connection; if that ever changes,
  this guard is not sufficient on its own.
- **Storage version.** `CREATE TRIGGER` needs a database at storage version
  `v2.0.0` or higher. Both routes into the tracked set refuse before tracking —
  `dbsp_track` and `create_view`'s source auto-tracking — with one readable
  error naming the migration; the sweep's own check sits below the deferral, so
  a user holding a transaction open never gets it out of their COMMIT or
  ROLLBACK. The engine's own message is
  `Binder Error: CREATE TRIGGER is only supported for storage versions v2.0.0
  and higher`, from the install, on every statement. Files written by the 2.0
  wheel are `v2.0.0+`; a file written by 1.5.4 is `v1.0.0+` and must be rewritten
  (`ATTACH ... (STORAGE_VERSION 'v2.0.0')` + `COPY FROM DATABASE`, then move the
  `.dbsp_spill/` sidecar directory alongside the new file). Verified end to end:
  the restored views match plain SQL and the triggers install on the next
  statement.
- **A write that matches no rows costs one scoped scan.** The bodies evaluate
  the ingest scalar once per transition-table row, so a zero-row write
  evaluates it zero times — and "the trigger fired and nothing changed" is then
  indistinguishable from "no trigger fired at all". The commit reconciles by
  scanning the statement's target table (H1 scoping keeps it to that one
  table). Pinned by `exact deltas: zero matching rows costs one SCOPED scan`.
- **A DETACH must not be preceded by the sweep.** Reading a catalog's version
  joins it to the statement's transaction, and `DETACH` refuses to run against
  a catalog the transaction has touched. `statement_detaches()` skips the sweep
  for that one statement. A keyword sniff is the right instrument here and the
  wrong one for detecting DDL: a miss costs that same loud error, never silent
  staleness.

## Buffering and rollback

- images are buffered per transaction, keyed by canonical
  `catalog.schema.table`, old at weight −1 and new at +1;
- a row inserted and then deleted in one transaction nets to zero and never
  reaches a view;
- an update chain collapses to the first old image and the last new image;
- `TransactionRollback` clears the buffer, so a rolled-back transaction leaves
  views untouched — and the sink rows the bodies wrote roll back with it;
- a conversion failure calls `mark_delta_unknown()`, which forces that commit
  to reconcile by scan instead of applying a partial delta.

Bodies run on **execution threads**, and an aggregate over a large transition
table may be parallel, so more than one thread can be inside
`buffer_trigger_delta` at once — it takes a mutex.

## Measurements

Correctness, on this tree (`ninja` build, `-j8`, stock engine
`v2.0.0-alpha39998 / a00803f768`):

| Run | Result |
|---|---|
| `ctest -j4` | **45/45 passed**, 61.0 s |
| `DBSP_TEST_VERIFY_VECTORS=1 ctest -j4` | **45/45 passed**, 55.3 s |
| `test_trigger_source` alone | **27 cases, 575 assertions** |
| `test_dml_shapes` alone | **10 cases, 352 assertions** |

On the PyPI wheel `duckdb==1.6.0.dev379`
(`v2.0.0-alpha39998 / a00803f768 / Cyanoptera`), via
`NumPad_App/.scratch/probe_a5.py`:

- `dbsp_query` equals plain SQL after `INSERT ... VALUES`, `UPDATE`, `DELETE`,
  `COPY ... FROM` a CSV and `INSERT ... SELECT`;
- `dbsp_stats()` reports `trigger_syncs 5`, `trigger_rows 12`,
  `captured_delta_syncs 4`, `scan_syncs 2`, and carries no
  `delta_source_mode` / `capture_guard_fallbacks`;
- after close and reopen with autopersist, the triggers are back and the view
  still matches plain SQL.

Migration from a database written by the 1.5.4 build: the file is storage
version `v1.0.0+` and `CREATE TRIGGER` is refused on it, so the install throws.
After `ATTACH ... (STORAGE_VERSION 'v2.0.0')` + `COPY FROM DATABASE` and moving
the `.dbsp_spill/` directory across, the views restore, the triggers install,
and edits are served by exact deltas (`NumPad_App/.scratch/probe_upgrade.py`).

Throughput, NumPad `medium` suite, config `E_20_trigger`: see
`NumPad_App/docs/benchmarks/2026-09-03-duckdb-2.0-alpha-comparison.md`.

## History

The trigger source began as a spike behind `DBSP_DELTA_SOURCE=trigger`
alongside two other delta sources. On 2026-09-03 the owner made it the only
one, and the other two were deleted:

| Removed | Lines | What it was |
|---|---:|---|
| `include/dbsp_write_capture.hpp` | 656 | predictive pre-image capture: rewrote a whitelisted UPDATE/DELETE/INSERT into a SELECT that read the old images and computed the new ones |
| `include/dbsp_plan_tee.hpp` | 506 | an `OptimizerExtension` that widened a DML plan and teed the rows it actually processed, for shapes the pre-image SELECT declined |
| `include/dbsp_engine_hook.hpp` | 202 | consumer for a patched engine's transaction-modification callback |
| `patches/` (2 files) | 719 | the engine patch itself, and its 1.5.4 archive |
| `scripts/build_engine_wheel.sh` + `.github/workflows/engine-wheel.yml` | 259 | the patched-wheel build machinery |
| capture/tee state in `dbsp_context_state.hpp` | ~700 of 1321 | `TeeCapture`, `try_write_capture`, `apply_captured`, the commit guard, the G2 LocalStorage scan |
| capture-mechanics tests | ~930 | `test_write_capture.cpp`, `test_engine_hook.cpp`, `test_engine_hook_consumer.cpp`, `bench_write_capture.cpp`, and the plan-shape canaries in `test_engine_assumptions.cpp` |

Net over the whole transition: **−4,040 lines** across 44 files
(`git diff --shortstat 7549a02..HEAD`: +2,118 / −6,158, taken with this
commit itself in the range — a SHA cannot be quoted here without going stale
the moment it is written), and the fork stopped being a fork of DuckDB — stock engine, stock
PyPI wheel, a CI that can build against a public one.

What was NOT deleted: the scan-and-diff reconcile (`sync_tables` / `sync_all`),
which is the safety net behind every route out of "I do not know what this
transaction wrote"; the `ParserExtension` for `CREATE MATERIALIZED VIEW`, which
is DDL and not capture; and the `captured_delta_syncs` counter, which now
counts trigger-fed deltas applied without a scan.

Tests that asserted CDC correctness through the capture path were ported rather
than deleted: `test_plan_tee.cpp` became `test_dml_shapes.cpp` (same shapes,
now asserted through the trigger path), and the differential matrix in
`test_auto_cdc.cpp` kept every case whose subject was the answer rather than
the mechanism. Deleted with a stated reason: the upsert cases (the engine now
refuses upserts on a tracked table — the refusal itself is pinned instead), the
commit-guard counter case (the guard is gone), and the forced-scan differential
(its kill switch is gone; the scan arm is now reached through
`dbsp_auto_sync(false)` + an explicit `dbsp_sync`).

## Follow-ups

- **An open transaction that loses its triggers.** `DROP t; CREATE t (same
  columns)` inside a transaction takes the bodies with the old table and leaves
  a same-shaped one behind: the fingerprint and the tracked count both stand
  still, so only the catalog can tell. It is detected at the first sweep AFTER
  the transaction ends, which is when the `duckdb_triggers()` read can run
  (an earlier version of this note said "not detected until commit"; before the
  sweep stopped short-circuiting on its own install record it was never
  detected at all, and the table stayed triggerless for the life of the
  process). Inside the transaction, correctness falls back to
  `sync_tables(touched)`. The residual: a write that leaves `saw_statements`
  false inside such a transaction would take the "nothing fed, no statement
  seen, so nothing was written" early return with no scan. Not reproduced —
  every write path measured here runs as a statement, so the combination looks
  unreachable — and forcing a scan on every transaction that saw a trigger
  install was rejected because it would make each `dbsp_track` cost a full
  `sync_all`.
- **A baseline is only "seeded" once something has scanned it.** The public
  `dbsp_track` leaves it empty on purpose and expects a `dbsp_sync`;
  `TrackedTable::baseline_seeded()` is what lets `create_view` tell "empty
  because nothing scanned it" from "empty because the table is empty" and seed
  it itself. Anything else that replays a baseline as though it were table
  content must ask the same question.
- **`dbsp_untrack` does not exist**, so the sweep's drop-DDL branch runs only
  when a table stops being tracked some other way (rollback of a `dbsp_track`),
  and is otherwise unexercised.
