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
`capture_.triggers_installed` makes that one commit reconcile by scan, and
every transaction that begins afterwards is served by triggers. Cost: one
scan-and-diff, once, per newly tracked table.

**Read that flag narrowly.** It is consulted only on the TRIGGER-FED commit
branch, so it is not what covers a transaction that writes a table and then
TRACKS it: that write is pre-trigger, and the table was untracked when the
statement folded, so `capture_.touched` never names it either and the commit
reaches the fallback branch's "read-only commit" early return.

What covers it is the baseline state and the SHAPE OF THE HOOK. Such a table is
DEFERRED and TAINTED, so no other connection's scan can establish it (see
*Whose rows are missing* below); and the reconcile sweep runs from a
DESTRUCTOR, so it fires on every path out of `TransactionCommit` including that
early return. At the deferring transaction's own commit the watermark has
cleared, the sweep names the table, and the scan runs. Pinned end to end by
`third_party_scan` and `tainted_rollback`.

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

Correctness, measured 2026-09-04 on this tree (`ninja` build, `-j8`, stock
engine `v2.0.0-alpha39998 / a00803f768`). Wall times are one run each on a
laptop and drift by tens of percent between runs; they are here to say the
suite is minutes, not hours:

| Run | Result |
|---|---|
| `ctest -j4` | **45/45 passed**, 118.7 s |
| `DBSP_TEST_VERIFY_VECTORS=1 ctest -j4` | **45/45 passed**, 92.6 s |
| `DBSP_STRICT_INTERNAL_QUERY=1 ctest -j4` | **45/45 passed**, 87.8 s |
| `test_trigger_source` alone | **35 cases, 1002 assertions** |
| `test_dml_shapes` alone | **10 cases, 352 assertions** |

On the PyPI wheel `duckdb==1.6.0.dev379`
(`v2.0.0-alpha39998 / a00803f768 / Cyanoptera`), via
`NumPad_App/.scratch/probe_a5.py`:

- `dbsp_query` equals plain SQL after `INSERT ... VALUES`, `UPDATE`, `DELETE`,
  `COPY ... FROM` a CSV and `INSERT ... SELECT`;
- `dbsp_stats()` reports `trigger_syncs 5`, `trigger_rows 12`,
  `exact_delta_syncs 4`, `scan_syncs 2`, and carries no
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

**2026-09-04 — the seeding state was folded back into one model.** Six weeks of
individually-correct fixes had left "is this baseline trustworthy?" stored five
ways across three files and two lifetimes: `deferred_`, `baseline_seeded_` and
`provisional_watermark_` on the table, a sticky per-CONNECTION
`unseeded_baseline_`, and a `ViewReadBlock::Kind` re-derived at the read gate.
The per-connection copy was the weakest — it named no table, so its debt could
only be paid by a full `sync_all`, and every awkward construct in
`TransactionCommit` existed to compensate: a `std::function` trampoline
installed at LOAD to route one boolean across an include edge, a commit-time
widening that set "we saw statements" on transactions that had seen none, a
`settle()` lambda, and a rollback special case. All of it is gone; the section
above describes what replaced it. Two defects the redundancy was hiding are
fixed with it (a scan inside the deferring transaction retiring the debt; the
read path refusing where it could have repaired) — see `CHANGELOG.md`.

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

Net over the whole transition: **−2,813 lines** across 45 files
(`git diff --shortstat 7549a02..HEAD`: +3,397 / −6,210, taken with this
commit itself in the range — a SHA cannot be quoted here without going stale
the moment it is written), and the fork stopped being a fork of DuckDB — stock engine, stock
PyPI wheel, a CI that can build against a public one.

What was NOT deleted: the scan-and-diff reconcile (`sync_tables` / `sync_all`),
which is the safety net behind every route out of "I do not know what this
transaction wrote"; the `ParserExtension` for `CREATE MATERIALIZED VIEW`, which
is DDL and not capture; and the `exact_delta_syncs` counter, which now
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

## Concurrency notes, and what is still open

Most of what follows is CLOSED — a design note kept because the reasoning is
load-bearing, with the commit that closed it. **Six** items are genuinely
open and say so: *an open transaction that loses its triggers* (a residual that
has not been reproduced); *`dbsp_untrack` does not exist* (an unexercised
branch); the two `QueryBegin` internal scans, whose `AllowedInTxn` whitelist is
a judgement rather than a proof; the provisional read gate's availability cost;
the strict switch's structural blindness to commit hooks; and *a concurrent
writer that never deferred anything* (a wrong answer, measured, whose fix is an
owner decision).

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
- **One per-table baseline state, and nothing else.** How far a tracked
  table's baseline can be trusted is `TrackedTable::Baseline`, a single enum on
  the per-INSTANCE table:

  | state | what it means | what serves it |
  |---|---|---|
  | `Unseeded` | nothing has scanned it; empty BECAUSE NOTHING SCANNED IT, which is not "empty because the table is empty" | the public `dbsp_track()` leaves it here on purpose and documents `dbsp_sync()` as the fill |
  | `Deferred` | a seeding scan was ASKED FOR and could not run: a user transaction was open, and an internal connection cannot see its uncommitted rows | the commit hook's sweep, once the deferring transaction has ended |
  | `Provisional(watermark)` | seeded correctly from committed storage while ANOTHER connection held a transaction open, so it may be short by what that transaction wrote before the table was tracked | the same sweep, once every transaction alive at seed time has ended |
  | `Seeded` | established, nothing outstanding | — the only state that serves an exact delta |

  Exactly two places ask:

  1. **APPLY** — `apply_captured_deltas` refuses any table that is not
     `Seeded` (`serves_exact_delta()`) and hands it back to the commit, which
     reconciles it by scan at that same commit. Without this, another
     connection's commits applied their deltas onto an empty baseline and read
     the deltas alone for the length of the window — `view 1.0 / sql 11.0`,
     `2.0 / 12.0`, `3.0 / 13.0` over three commits, self-healing later and a
     wrong answer with no error while it lasted.
  2. **READ** — `view_read_block` walks a view's sources transitively and
     returns the WORST state in the tree; `EnsureViewReadable`
     (`src/dbsp_extension.cpp`) throws unless it is `Seeded`, with a different
     remedy for each state — `dbsp_sync()` repairs UNSEEDED, and only ending
     the transaction repairs DEFERRED. A pure read during
     the window returned the unseeded value too — `dbsp_query('mv')`
     `[(None,)]` against plain SQL `10.0`, on the deferring connection and on
     every other one, on `:memory:` and on a file; `dbsp_changes` was worse
     still, serving `[(None,-1),(10.0,1)]` on a connection whose `dbsp_query`
     said NULL.

  **Readiness, and why any connection can pay.** `Deferred` and `Provisional`
  each carry a watermark — a transaction-manager start timestamp their debt's
  transaction has passed once it ends — and both are compared against
  `DuckTransactionManager::LowestActiveStart()` by one predicate. A table is
  scanned only once its watermark has cleared, so a scan can never establish a
  baseline that a still-open transaction would have changed. That is what makes
  the debt safe to pay from ANY connection's commit hook, including a commit
  that touched nothing at all — which is the case the provisional reproduction
  needs, where the connection that repairs the view is a bare `SELECT 1` on the
  other connection. It is also what lets the READ path attempt the repair
  (guarded by "the reader holds no transaction of its own"), so the first read
  after the window answers instead of refusing.

  `Unseeded` is deliberately NOT swept. `dbsp_track()`'s contract is an
  explicit `dbsp_sync()`, and scanning it here would establish a baseline
  without `seed_baseline`'s concurrency check
  (`mark_provisional_if_concurrent`) — a table tracked while another connection
  held a write open would be marked trusted while short.

  **`dbsp_sync()` inside the deferring transaction cannot pay the debt, and
  says so.** The scan it runs opens its own connection and cannot see that
  transaction's uncommitted rows, so establishing from it would serve a view
  short by exactly those rows. `retire_scanned_baseline` refuses to establish
  while the CALLER holds a transaction open, and `dbsp_sync()` reports what is
  still owed (`… ; N baseline(s) still owed a seeding scan …`) rather than the
  plain "Synced" of a call that worked. Called with no transaction open it
  establishes normally, which is the documented repair for a table left empty
  by `dbsp_track()`. Pinned by `in_window_sync` in
  `test/python/test_create_view_seeding.py`.

  **Only a scan that could see everything may establish a baseline.**
  `finish_rebuild()` installs CONTENT and never touches the trust state;
  `install_rebuild(establishes)` takes a MANDATORY flag and only the seeding
  scan passes `true`. Everything else — a refresh from `rebuild_all_views`, a
  restore-pending materialization — passes `false` and leaves the trust state
  to `retire_scanned_baseline`, which is the ONE rule that retires an untrusted
  baseline and asks all three questions above. Measured without that rule:
  crash recovery's `resync_tracked_tables`, which runs from `QueryBegin` and
  can be inside the very transaction that deferred the seed, marked the
  baseline trusted from a committed-only read and the commit then reconciled
  nothing — `view 10.0` where SQL read `13.0`.

  **The watermark never lowers.** Two connections can defer the same table, and
  the debt is waiting on whichever transaction ends LAST, so
  `mark_seed_deferred` takes the MAXIMUM of the watermark it holds and the one
  offered. Overwriting let an older transaction's lower watermark clear while
  the newer transaction was still open, and the reconcile then established the
  baseline without its rows — `view 10.0` against SQL `13.0`, and it never
  healed, because the write predated the triggers and so fed nothing. Pinned by
  `watermark_never_lowers`.

  **Whose rows are missing decides who may establish a DEFERRED baseline.**
  The rows a deferred baseline lacks belong to the transaction that deferred
  it, and they reach a view one of exactly two ways: its triggers report them
  at its commit, or `capture_.touched` names the table and its commit scans it.
  Both need the table to have been TRACKED before the write — a statement that
  writes an untracked table folds without naming it, and the triggers do not
  exist until the transaction ends. (Tracked, not triggered: a write to a
  tracked table whose triggers are not installed yet is still named by
  `touched`.)

  So the deferral carries one bit, `TrackedTable::pre_trigger_rows`, taken at
  the moment the table JOINS the tracked set by asking the engine whether the
  caller's transaction already holds ANY uncommitted change:
  `DuckTransaction::ChangesMade()` — `undo_buffer.ChangesMade() ||
  storage->ChangesMade()`, i.e. deletes and updates of committed rows, catalog
  changes and sequence use (the undo buffer) or appends (local storage). It
  reads the caller's own transaction state, not committed storage, so the
  internal-connection law does not apply. The probe and the readiness
  watermark come from ONE lookup of the same `DuckTransaction`
  (`CDCManager::probe_deferring_transaction`), so they cannot describe
  different transactions; if that lookup cannot be made at all (the table's
  catalog is gone, or has no DuckDB transaction manager) the deferral is
  REFUSED with an error rather than recorded — there is no watermark to retire
  it by, and a debt that can never be retired would hold the table DEFERRED
  for the life of the process.

  **The rule is transaction-wide, not per-table, and that is deliberate.** The
  engine records which table each uncommitted change belongs to
  (`DeleteInfo`/`UpdateInfo`/`AppendInfo` carry a `DuckTableEntry*`) but only
  inside the undo buffer, whose iteration and storage are private
  (`UndoBuffer::IterateEntries`, `DuckTransaction::undo_buffer`); the per-table
  locks a write takes (`DuckTransaction::active_locks`) have no accessor. The
  first version used the one per-table signal that IS public,
  `LocalStorage::Find(table)`, and it was wrong: `Find` reports a
  transaction-local APPEND buffer and nothing else, so a DELETE or UPDATE of
  committed rows — which goes to the undo buffer and creates no local storage —
  read UNTAINTED, a third connection's `dbsp_sync()` established the baseline
  at the pre-change value, and the deferring commit found nothing owed:
  `view 13.0` against SQL `10.0`, permanently, for both shapes. Pinned by
  `tainted_delete_third_party_scan` and `tainted_update_third_party_scan`.
  What the coarser rule costs: a transaction that has written ANYTHING — another
  table, a `CREATE TABLE`, a sequence — and then tracks a table is tainted, so
  reads of views over that table are refused until it ends. That is
  availability for one transaction's length on a rare shape; a wrong answer is
  worse than a refusal, and the rule is never approximated from statement
  text.

  | | who may establish the baseline | reads during the window |
  |---|---|---|
  | UNTAINTED — the transaction had changed nothing when it tracked the table | ANY scan: committed state plus the deltas that transaction's own commit contributes is exactly right | served |
  | TAINTED — the transaction already held uncommitted changes when it tracked the table | only a scan taken once that transaction is GONE (`ready_watermark_cleared`) | refused |

  A tainted window has a second cost besides the refused reads: every writing
  commit on every connection finds the table not SEEDED, has its exact delta
  refused on the apply path, and pays a scan-and-diff of the table instead —
  once per such commit, until the deferring transaction ends.

  The untainted row is the common case and is what keeps a second connection
  reading correct answers during a deferral window rather than being refused —
  `cdc: an unseeded baseline is never served to another connection` pins 110.0,
  not an error, and `untainted_third_party_scan` pins the same shape through a
  third connection's `dbsp_sync()`.

  The tainted row is the rare write-then-track-in-one-transaction shape, and
  before the bit existed it was a permanent wrong answer with no error:

  ```
  A: BEGIN; INSERT INTO t VALUES (2, 3.0);          -- t untracked, no triggers
  A: dbsp_create_view('mv', 'SELECT sum(v) FROM t') -- t tracked, DEFERRED
  B: SELECT * FROM dbsp_sync()                      -- B holds no transaction
  A: COMMIT
     dbsp_query('mv') -> 10.0     SELECT sum(v) FROM t -> 13.0    permanently
  ```

  B's scan reads committed state, holds no transaction of its own, and used to
  mark the baseline SEEDED — after which A's own commit found nothing owed.
  Now it installs the CONTENT and leaves the state DEFERRED, so A's commit
  sweep finds the table by name and scans it to 13.0. Pinned by
  `third_party_scan`; `tainted_rollback` pins the other exit, where the
  unreportable rows are rolled back and the first read afterwards establishes
  the baseline at committed state.

  **The seeding concurrency check is NOT asked on a reconcile, and cannot be.**
  `mark_provisional_if_concurrent` has exactly ONE call site, `seed_baseline`.
  It needs a reference start timestamp to mean "older than this", and on the
  seeding path that is the caller's own transaction. A reconcile has no caller
  transaction, and a timestamp probed on the spot counts the caller's OWN
  statement transaction as an older concurrent one — measured,
  `lowest_active_start=36 mine=53(probe)` for a bare `dbsp_sync()`, which then
  marked the table PROVISIONAL and refused every read of the view it had just
  repaired. The stored readiness watermark needs no reference of its own, and
  it plus the taint bit is the whole of the reconcile's concurrency story for
  the transactions that DEFERRED a seed on the table. It answers only for
  those — *a concurrent writer that never deferred anything*, below, is the
  shape it cannot see.

  **Ordering note.** PROVISIONAL retirement used to run in `sync_tables`,
  after `propagate_changes_multi`; it now runs inside the scan, with the
  per-table lock released before propagation. So a reader can observe SEEDED
  slightly before the repair delta reaches the views — the same window DEFERRED
  already had, and narrower than the window the read gate is there to close.

  **A reconcile that FAILS must not retire the debt.** `sync_tables` returns
  false when a table it was asked about was not scanned, reports it through
  `record_error_best_effort` and on stderr, and nothing is retired on a failed
  scan — the per-table state stands and the next commit tries again. Clearing
  first meant one failed scan left the baseline empty for the life of the
  connection, silently.

  The commit hook is therefore one sweep plus the fallbacks it does not
  replace. `TransactionCommit` still reaches `sync_all` from three places —
  `triggers_installed`, `unknown_writes` on the trigger-fed path, and
  `!know_all_writes` on the fallback path — exactly as it did before. What
  changed is that a seeding debt no longer FORCES the `unknown_writes` route:
  the sweep pays it by name, so the full scans are taken only when a write
  genuinely cannot be attributed to a table. Pinned by
  `test/python/test_unseeded_read.py`, `test_provisional_baseline.py`,
  `test_create_view_seeding.py`, `test_reconcile_telemetry.py` and, in ctest,
  `cdc: one baseline state drives both the apply path and the read path`, which
  flips the state once and asserts both consultation points flip with it.

  This is the third defect family of the internal-connection kind in this work
  — the sweep's DDL, the sweep's catalog-version read, and the seeding scan.
  **The law is enforceable** rather than a rule in a comment: every helper that
  opens an internal connection takes an explicit `InternalReadPolicy` and
  `DBSP_STRICT_INTERNAL_QUERY=1` turns a `Forbidden` call inside an open user
  transaction into a throw naming the site (see *The internal-connection law is
  enforceable* below). The seeding scan is `Forbidden`; the two scans that run
  from `QueryBegin` — `rebuild_all_views` and `materialize_all_deferred` →
  `materialize_deferred_locked` — are whitelisted at their call sites, because
  both REFRESH a baseline that already exists rather than establishing one.
  That whitelist is a JUDGEMENT, not a proof: the commit reconcile appears to
  cover the window they open, and nothing pins it. What has changed is that
  anything NEW opening an internal connection has to answer the question in
  code.
- **A baseline is only "seeded" once something has scanned it.** The public
  `dbsp_track` leaves it empty on purpose and expects a `dbsp_sync`;
  `TrackedTable::established()` is what lets `create_view` tell "empty
  because nothing scanned it" from "empty because the table is empty" and seed
  it itself. Anything else that replays a baseline as though it were table
  content must ask the same question.
- **OPEN (narrowed) — the read gate still costs AVAILABILITY while the window
  is open.** A reader that holds no transaction of its own now RUNS the repair
  sweep before refusing, so any baseline whose watermark has cleared is scanned
  and served rather than refused. What remains is the window itself: while the
  deferring or seeding transaction is still alive, `dbsp_query` /
  `dbsp_changes` refuse on every connection. That is required for a reader
  INSIDE the deferring transaction, whose own uncommitted writes the baseline
  cannot contain. It is NOT required for an ordinary autocommit reader on
  another connection: that reader's snapshot cannot see the deferring writes
  either, so the committed-state answer the baseline already holds is exactly
  right for it — plain SQL on that connection reads `10.0` and the baseline
  holds `10.0`. The gate does not distinguish the two, so it refuses both.

  Nothing is wrong; something is unavailable. A long-running writer transaction
  will expose it: for as long as it is open, every reader of a view whose
  source was tracked during that window is refused, where most of them could
  have been served correctly.

  Candidate refinement, not costed: serve an autocommit reader whose own
  catalog snapshot cannot see the deferring transaction's writes, and refuse
  only the reader inside it (or one whose snapshot postdates the commit but
  whose baseline has not yet been reconciled). It needs a way to compare the
  reader's snapshot against the provisional watermark, which the transaction
  manager exposes — `DuckTransaction::start_time` on the reader versus the
  table's watermark — but the reasoning has to be got right before the gate is
  loosened, and a wrong answer is worse than an error.

- **OPEN — a concurrent writer that never deferred anything.** The readiness
  watermark a DEFERRED table carries is the deferring transaction's own start
  + 1, and it is consulted only when the deferral is tainted. It therefore
  covers exactly one transaction — the one that deferred a seed on that table
  (for PROVISIONAL, every transaction alive at ITS seed time). A transaction on
  another connection that wrote the table while it was untracked, and deferred
  nothing, is invisible to it. Two shapes, both measured on the round-4 tree,
  both permanent wrong answers with no error:

  ```
  -- tainted deferral, younger writer
  A: BEGIN; INSERT INTO t VALUES (2, 3.0);           -- t untracked
  D: BEGIN; INSERT INTO t VALUES (3, 4.0);           -- D begins AFTER A; t still untracked
  A: dbsp_create_view('mv', 'SELECT sum(v) FROM t')  -- t tracked, DEFERRED, tainted, watermark = A.start + 1
  A: COMMIT                                          -- LowestActiveStart() = D.start > watermark: "cleared";
                                                     -- A's own sweep scans committed state: 13.0, without D
  D: COMMIT                                          -- no trigger fired for D's INSERT (none existed when it ran);
                                                     -- `touched` never named t (untracked when the statement folded)
     dbsp_query('mv') -> 13.0    SELECT sum(v) FROM t -> 17.0    then 14.0 / 18.0 after the next edit

  -- untainted deferral, any concurrent writer
  A: BEGIN; SELECT count(*) FROM t;                  -- A has written nothing
  D: BEGIN; INSERT INTO t VALUES (3, 4.0);           -- t untracked
  A: dbsp_create_view('mv', 'SELECT sum(v) FROM t')  -- t tracked, DEFERRED, UNTAINTED
  B: SELECT * FROM dbsp_sync()                       -- untainted: any scan may establish -> SEEDED at 10.0
  A: COMMIT; D: COMMIT                               -- D's row is reported by nothing
     dbsp_query('mv') -> 10.0    SELECT sum(v) FROM t -> 14.0    then 11.0 / 15.0
  ```

  Why the watermark cannot see it: in the first shape D's start is above A's
  watermark, so D's openness never holds the scan back; in the second the
  watermark is not consulted at all, because the untainted rule lets any scan
  establish. What this path lacks is the seeding path's concurrency check: a
  NON-deferred seed runs `mark_provisional_if_concurrent` and waits on every
  older open transaction, while a deferred seed's establishing scan runs later,
  from a hook with no caller transaction to be relative to, and asks nothing
  about other writers. Pre-existing — every scan that has ever retired
  DEFERRED had this blind spot; the taint rule (fix 3) closed the
  third-party-scan wrong answer, not this one. Measured 2026-09-04; not pinned.

  Fix direction, and its cost: take the DEFERRED watermark at tracking time as
  a start timestamp newer than every transaction alive at that moment
  (`probe_new_start_timestamp`, exactly as PROVISIONAL does) instead of the
  deferring transaction's own start, and consult it for untainted deferrals
  too. That holds the establishing scan until every transaction that could
  have written the table untracked is gone, which closes both shapes. It also
  refuses the untainted third-party establish for as long as ANY transaction
  that was open at tracking time is still open — the deferring transaction
  included, so the shape `cdc: an unseeded baseline is never served to another
  connection` pins at 110.0 becomes a refusal for the length of A's
  transaction, and so does `untainted_third_party_scan`. That is the trade this
  design has refused so far (a rare wrong answer against a common outage); it
  is an owner decision, not a fix-round edit.

- **The strict switch cannot see a commit hook.** `DBSP_STRICT_INTERNAL_QUERY=1`
  fires on `user_transaction_open(context)`, and DuckDB clears the transaction
  context BEFORE running its commit callbacks
  (`duckdb/src/transaction/transaction_context.cpp:62`), so auto-commit is true inside
  every commit hook by construction. A violation of the internal-connection law
  made from a commit hook is therefore structurally invisible to the switch: it
  bites only on calls made DURING a statement. The `Forbidden` markings on the
  two provisional reconciles are still the honest state — they say what the
  site requires — but there they are documentation, not enforcement. Anything
  that starts reading committed-only state from a commit hook has to be
  reviewed by hand.

- **`dbsp_untrack` does not exist**, so the sweep's drop-DDL branch runs only
  when a table stops being tracked some other way (rollback of a `dbsp_track`),
  and is otherwise unexercised.
- **A table tracked while ANOTHER connection holds a write open.** Closed by a
  per-INSTANCE transaction watermark. The shape, measured: connection 1 runs
  `BEGIN; INSERT` on an UNTRACKED table and leaves the transaction open;
  connection 2 tracks the table (or creates a view over it) and seeds the
  baseline from committed state — correctly, since connection 2 has no
  transaction of its own and cannot see connection 1's rows; connection 1 then
  commits. No trigger fired for that INSERT (there were no triggers when it
  ran) and connection 1's own `touched` never named the table, so the delta
  never happened: the view read `10.0` against SQL `13.0`, then `14.0` against
  `17.0`, and never healed. It was NOT the cross-connection defect the
  apply-path gate closed, and the two looked alike: there the baseline was
  UNSEEDED and another connection's delta was applied onto nothing; here the
  baseline is seeded correctly and it is the other connection's PRE-TRACKING
  write that no one accounted for. It was also invisible to `DBSP_DEBUG_SEED`,
  which is per-context and reported `user_txn_open=0` for connection 2 — the
  honest answer to the wrong question.

  DuckDB's transaction manager publishes what is needed:
  `DuckTransactionManager::LowestActiveStart()`, the smallest start timestamp
  among a database's ACTIVE transactions (and a value larger than any real
  start once none are left). At seed time `mark_provisional_if_concurrent`
  compares it against the seeding transaction's own `start_time`; a lower value
  means a transaction OLDER than this one is open, so the table is marked
  PROVISIONAL with a watermark taken by starting a transaction on the spot and
  rolling it straight back — a start timestamp newer than every transaction
  alive at that moment.

  While provisional, the apply path refuses the table exactly as it refuses an
  unseeded one — one enum, one predicate — so it rides the `failed` →
  scan-reconcile route on every connection's commit. The retirement is a sweep in the commit hook
  (`reconcile_untrusted_baselines`, run from a destructor so it fires on every
  path out of the hook, and AFTER this commit's own deltas were handled so it
  cannot double-count): once `LowestActiveStart()` has
  risen to the watermark, every transaction that existed at seed time has ended
  and one scan pays the debt. It has to be a sweep and not just the apply path,
  because in the reproduction the connection that repairs the view is the one
  that writes NOTHING — at connection 1's own commit, connection 1's
  transaction is still active, so the repair falls to the next statement on
  connection 2, a bare `SELECT 1`.

  Cost when nothing else is open: `LowestActiveStart()` is the seeding
  transaction's own start, the comparison is false, the table is never
  provisional, and the sweep's steady state is one atomic load per commit.
  Pinned both ways by `test/python/test_provisional_baseline.py`, which asserts
  six later edits cost **0** scans and **6** exact deltas in the solo case.
  `dbsp_stats()` reports the live count as `provisional_tables`. (`578a79f`.)

  Residual, stated rather than hidden: a transaction that BEGINS during the
  seeding statement is not covered by "older than mine". Its writes to a
  still-untriggered table are the trigger-install window, which that
  transaction's own commit answers — by `capture_.triggers_installed` on the
  trigger-fed branch, and otherwise by the baseline state, which keeps such a
  table out of SEEDED until the transaction ends so the commit sweep names it
  (see *Read that flag narrowly* above). A catalog served by a non-DuckDB
  transaction manager has no watermark to take; the table is never marked
  there, which is exactly the behaviour before this gate.
- **The internal-connection law is enforceable.** Not an assertion inside
  `InternalQueryGuard` — that has 39 call sites, no `ClientContext` to ask, and
  legitimate exceptions that would false-positive. Instead every SITE that
  opens an internal `duckdb::Connection` to read a USER table or to run DDL
  declares an explicit `InternalReadPolicy{Forbidden, AllowedInTxn}` and a site
  name. Not only the streaming helpers: the watermark reads (`live_watermark`,
  `fold_fresh_baseline`, `save_checkpoint`, `checkpoint_valid`,
  `register_arrangements`) are the same
  committed-only-read-during-an-open-transaction shape and carry it too, and so
  does every write to DBSP's own bookkeeping tables. `docs/TESTING.md` lists
  every site with its policy. Under
  `DBSP_STRICT_INTERNAL_QUERY=1` a `Forbidden` call made while
  `user_transaction_open(context)` throws an `InternalException` naming the
  site; `ctest` runs a third time with it set, the way
  `DBSP_TEST_VERIFY_VECTORS` does (`docs/TESTING.md`).

  Off by default on purpose: such a call is a bug the commit reconcile usually
  papers over, and turning that paper-over into a crash in production would
  trade a wrong answer for an outage.

  Four exceptions, each whitelisted in code at its call site with its reason:
  the sweep's `duckdb_triggers()` presence read (a plain SELECT taking its own
  snapshot); `rebuild_all_views` and `materialize_deferred_locked`, both
  reached from `QueryBegin`, which REFRESH a baseline that already exists
  rather than establishing one; and user-invoked `dbsp_sync()` /
  `dbsp_sync('t')`, where committed storage is exactly what the caller asked
  for. The seeding scan is `Forbidden` and means it — `seed_baseline` already
  refuses to reach it inside an open transaction, so a throw there says that
  refusal has been bypassed.

  Proved to bite: flipping the `duckdb_triggers()` whitelist to `Forbidden`
  turns the strict run red (44/45, `trigger_source` failing with the law's own
  message); restoring it returns 45/45. (`009ec57`.)
- **A failed reconcile scan is visible from SQL.** `dbsp_stats()` carries
  `reconcile_failures` (a count) and `last_reconcile_error` (the message, in a
  new third `detail` column). This is the one way a view is left stale with the
  manager knowing it: the scan reports failure by RETURNING, not by throwing,
  so nothing propagates out of the commit hook and the only other trace is a
  stderr line an embedding host never sees. Pinned by
  `test/python/test_reconcile_telemetry.py` with round 5's own scenario — a
  view created inside an open transaction (seeding deferred, debt recorded),
  the source DROPped in the same transaction, and the COMMIT widening itself to
  pay a debt with a scan that cannot run. (`396fac4`.)
