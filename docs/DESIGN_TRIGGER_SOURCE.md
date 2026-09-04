# Trigger-fed delta source

Status: SHIPPED behind `DBSP_DELTA_SOURCE=trigger` (2026-09-03). Third delta
source alongside `docs/DESIGN_ENGINE_HOOK.md` (patched engine) and
`docs/DESIGN_WRITE_CAPTURE.md` (predictive capture). Off unless asked for.

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

`dbsp_trigger_ingest(key, weight, cols...)` is a new vectorised extension
scalar (`src/dbsp_extension.cpp`, `TriggerIngestScalar`). It is `VOLATILE` (so
it is never folded or cached) with `SPECIAL_HANDLING` null handling (row images
are full of NULLs; the default would have skipped them). It converts its chunk
of row images into a signed Z-set — reading typed vector data directly, exactly
like `engine_cdc_to_zset` — and writes it into
`DBSPContextState::engine_buffer_delta`, the **same** per-transaction buffer
the engine hook fills.

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
(*catalog lookups inside Commit are unsafe*, `dbsp_engine_hook.hpp`), and it
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

`CREATE OR REPLACE` makes re-tracking after a reopen free of "trigger already
exists" failures. The install record is per database and holds a **weak
reference** to its `DatabaseInstance`: keying it on the raw address alone was
wrong in exactly the way this fork has recorded before — DuckDB reuses freed
addresses, and consecutive test harnesses in one process inherited a stale
"already installed" list and ran with no triggers at all. Two tests failed on
that and nothing else; it is a real bug class, not a theoretical one.

### No double counting

In trigger mode `register_engine_hook` does not register, and the capture stack
and plan tee disarm through `exact_delta_source_active()`. As with the engine
hook, the flag that disarms them flips only on a **delivered** ingest, never at
install time — triggers that exist but never fire leave the capture stack armed
rather than silently dropping every commit. Pinned by
`trigger source: one insert counts exactly once`, which asserts a **sum**, not
a row count: a row delivered twice doubles the sum while leaving the row count
untouched.

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
| `ALTER TABLE ... ADD COLUMN` | **allowed** — and handled: the sweep regenerates the bodies (below) |
| `INSERT ... ON CONFLICT DO NOTHING` | allowed |
| `DROP TABLE` | allowed; takes the triggers with it, and a recreate + re-track reinstalls them |

Sources: `duckdb/src/planner/binder/statement/bind_merge_into.cpp:226-233` for
MERGE; the other two messages are the engine's own, reproduced in the shell on
this build.

So trigger mode does not merely add a delta source — it **removes upserts and
most schema changes from every table NumPad tracks**. Neither the engine hook
nor the capture stack costs anything like that. An earlier draft of this
document called MERGE "the one real price"; that was wrong, and the full list
is the thing to weigh.

For NumPad specifically: the single `MERGE INTO` site is
`api/integration/transforms/service.py:371`, targeting `land.<relation>` in the
data-integration landing schema, and nothing tracks `land.*`. So none of this
bites today — but it is a standing constraint on every tracked table.

### Keeping the bodies in step with the table

A generated body encodes the column list literally, so anything that moves a
tracked table's schema makes every installed body stale. Most such statements
the engine refuses outright (above); `ADD COLUMN` does not, and `DROP TABLE`
silently takes the triggers away.

Neither moves the tracked-table **count**, which is what the cheap steady-state
check compares — so the first version of this sweep missed both, and both
produced wrong answers with no scan to catch them: a view read 5.0 where SQL
read 12.0 after an `ADD COLUMN`, and a dropped-and-recreated table stayed
tracked but permanently triggerless.

The sweep therefore keys its install record on a **column fingerprint** (name
and type of every column, in order) taken from the LIVE catalog — not from the
manager's cached schema, which is what the bodies were generated from and so
could never disagree with itself — and additionally verifies against
`duckdb_triggers()` that all three triggers are still there. A full reconcile
runs when the tracked-table count changes, and whenever the previous statement
began with `ALTER`, `DROP`, `CREATE`, `ATTACH` or `DETACH`. That keyword sniff
is deliberately over-inclusive: a false positive costs one catalog query, a
false negative costs silent wrong answers.

Regeneration lands the same way a first install does — `CREATE OR REPLACE`,
with the regenerating transaction marked so its commit reconciles by scan,
which is also what re-reads the table at its new width.

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

## Buffering and rollback

Identical to the engine hook by construction, because it is the same buffer:

- images are buffered per transaction, keyed by canonical
  `catalog.schema.table`, old at weight −1 and new at +1;
- a row inserted and then deleted in one transaction nets to zero and never
  reaches a view;
- an update chain collapses to the first old image and the last new image;
- `TransactionRollback` clears the buffer, so a rolled-back transaction leaves
  views untouched — and the sink rows the bodies wrote roll back with it;
- a conversion failure calls `engine_mark_unknown()`, which forces that commit
  to reconcile by scan instead of applying a partial delta.

One thing the trigger path has that the hook does not: bodies run on
**execution threads**, and an aggregate over a large transition table may be
parallel. `engine_buffer_delta` therefore takes a mutex.

## Measurements

Correctness, on this tree (`ninja` build, `-j8`):

| Run | Result |
|---|---|
| `ctest` (build/test, default mode) | **48/48 passed**, 175.6 s |
| `test_trigger_source` alone | **11/11 cases, 211 assertions** |
| `DBSP_TEST_VERIFY_VECTORS=1 ctest` | **48/48 passed**, 114.8 s |

47 of those 48 are the pre-existing suite; `trigger_source` is the new entry.
It is built **without** `DBSP_ENGINE_HOOK`, which is the point: nothing in it
can be served by the patched engine's callback.

On the **stock** PyPI wheel `duckdb==1.6.0.dev379`
(`v2.0.0-alpha39998 / a00803f768 / Cyanoptera`), via
`.scratch/probe_trigger_source.py` in NumPad_App:

- `delta_source_mode = 3`, triggers present as
  `dbsp_trg_items_{del,ins,upd}`;
- `dbsp_query` equals plain SQL after INSERT, UPDATE and DELETE;
- `trigger_syncs 4`, `trigger_rows 5`, `captured_delta_syncs 0 → 3`,
  `scan_syncs 0`, `capture_guard_fallbacks 0` — every commit served by an
  exact trigger-fed delta, no scan, no capture-stack activity;
- after close and reopen with autopersist, the triggers are back
  (`dbsp_trg_t_{del,ins,upd}`) and the view still matches plain SQL.

Throughput, NumPad `medium` suite, config `E_20_trigger`: see
`NumPad_App/docs/benchmarks/2026-09-03-duckdb-2.0-alpha-comparison.md`.

## What this would let us delete

This is the reason the spike exists. If trigger mode becomes the default, the
following stop having a justification. Counts are `wc -l` on this tree.

| Candidate | Lines | Why it can go |
|---|---:|---|
| `include/dbsp_write_capture.hpp` | 656 | predictive pre-image capture: the trigger reports facts, so there is nothing to predict |
| `include/dbsp_plan_tee.hpp` | 506 | optimizer tee for shapes design-1 declines |
| `test/unit/test_write_capture.cpp` | 518 | |
| `test/integration/test_plan_tee.cpp` | 293 | |
| `test/benchmarks/bench_write_capture.cpp` | 114 | |
| `include/dbsp_engine_hook.hpp` | 202 | the hook consumer |
| `test/unit/test_engine_hook.cpp` | 299 | |
| `test/integration/test_engine_hook_consumer.cpp` | 128 | |
| `patches/v2.0.0-alpha39998-dbsp-txn-callback.patch` | 371 | **the engine patch itself** |
| capture/tee state in `dbsp_context_state.hpp` | ~400 of 1291 | `TeeCapture`, `classify`, `try_write_capture`, `apply_captured`, the commit guard |
| **Total** | **~3,500** | against the 506 + 363 lines this source added |

The **real** prize is not the line count. It is the last row but one: with a
trigger source there is no forked engine. That removes

- `patches/` and `build.sh`'s `git apply` step,
- the `DBSP_ENGINE_HOOK` compile flag and the two CMake branches that carry it,
- the pinned `duckdb-python` fork ref and the locally built wheel NumPad
  depends on,
- and the CI-unbuildable problem recorded against this fork: CI could build
  against a **public** DuckDB wheel, which it cannot do today.

The write-capture stack's own justification is already gone: it exists for
write paths the binder never sees, and in 2.0 the Appender is no longer one of
them (`Appender::FlushInternal`, `duckdb/src/main/appender.cpp:614-627`, now
runs `INSERT INTO ... SELECT`).

### What has to be true first

Not decided by this spike; listed so the decision is not made on vibes.
The benchmark column now exists (NumPad `214f15db`) and is inside the band, so
item 1 is provisionally answered — on one pass, on battery, with the slowest
disk of the five runs. It is not yet settled.

1. **Throughput.** The benchmark column has to be inside the band. A trigger
   body pays a transition-table materialisation and a sink insert per statement
   that neither other source pays.
2. **`MERGE INTO`.** Removing it from tracked tables must be an acceptable
   product constraint, permanently.
3. **The user-visible catalog objects** must be acceptable — triggers and a
   sink table on the user's schema, in their exports and their `SHOW TABLES`.
4. **A soak.** `soak_differential` has never been run against trigger mode.
5. **The two unpinned coverage rows** (`INSERT ... SELECT`, `TRUNCATE`) must move
   into the C++ suite, and the drop-on-untrack path — written, never executed —
   needs an entry point to drive it and a test on it.

Until all four hold, this stays a mode, not the default, and nothing on the
deletion list gets deleted.
