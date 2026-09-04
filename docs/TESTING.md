# Testing Guide

How duckDBSP is tested, how to run the suites, and what each layer
covers. The single most important idea: **differential testing** —
after every batch of changes, an incrementally maintained view must
equal DuckDB's own answer for the same SQL. DuckDB is the oracle; the
engine never gets to grade its own homework.

## Running the suites

ctest registers **45** entries. Two of them are bench binaries registered as
smoke entries (`planner_eval_smoke`, `window_bench`). There is one build
configuration — the engine is stock and there is no build option to vary.

```bash
cd test/build_test
cmake .. && make -j8
ctest -j4                   # full suite, ~1 min
./test_planner_frontend     # the big differential suite on its own
```

The root build (`./build.sh`) also builds every test binary into `build/test`
from the same sources and options, so `ctest` from `build/` is equivalent and
saves a second ~2.6GB DuckDB build when disk is tight.

### `DBSP_TEST_VERIFY_VECTORS=1` — vector verification, suite-wide

```bash
ctest                                  # normal run
DBSP_TEST_VERIFY_VECTORS=1 ctest       # same suite under VERIFY_VECTORS
```

With the variable set to anything but `""` or `0`, every test binary runs with
DuckDB's `debug_verification_mode='verify_vectors'`. Under that mode
`DataChunk::VerifyInternal` checks at every operator boundary that each child
vector reports the same size as the chunk, and throws

```
DataChunk::Verify - size mismatch: vector N (VARCHAR) has size 0 but chunk has size 1
```

instead of quietly passing a malformed chunk along.

This exists because of a real wrong-answer bug. On DuckDB 2.0 a Vector carries
its own size and the deprecated `DataChunk::SetCardinality` no longer sizes the
children, so every table function that ended a scan callback with it emitted
chunks whose columns claimed to be empty — which made `IS NULL` false for every
row in the planner frontend (CHANGELOG, "DuckDB 2.0 alpha issues"). One test
armed the mode by hand and caught it; the other 48 sites were found by grep.
The switch is the standing version of that: any new stale-size site is loud in
whichever test first touches it.

Mechanics (`test/verify_vectors.hpp`): the mode lives in
`DBConfigOptions::global_verification_mode`, a process-wide static, so it is
armed once before `main()` from `catch2_main.cpp` — which every test binary
links, including the ones that open a raw `DuckDB db(nullptr)` and never build
a `DuckDBTestHarness`. The harness re-arms on each database open, because the
`read surface: chunks satisfy VERIFY_VECTORS` case in `test_extension_basic.cpp`
arms the mode itself and resets it to `none` on the way out (that case is the
regression pin for the original bug and is unchanged by the switch).

There is deliberately **no ctest label** for this: an environment variable
means CI runs the same suite twice with no second test registration. Both runs
are expected green; a failure only under the switch is a real latent bug, not
a test-harness artifact. `test/python/*.py` are not covered — they open their
own connections and are not in ctest anyway.

### `trigger_source` — the delta source

`test_trigger_source` (`test/unit/test_trigger_source.cpp`, registered as an
integration test because it needs the extension) is the differential oracle for
the one delta source: old images at −1, new at +1, insert-then-delete nets to
zero, update chains collapse to first-old/last-new, rollback discards
everything, multi-table transactions apply in one pass. Every case also
cross-checks `dbsp_query` against plain SQL, because a source that is
self-consistently wrong would pass a weight assertion.

```bash
cd test/build_test
./test_trigger_source       # 29 cases, 642 assertions
```

Beyond the oracle it pins the paths specific to this source: the C++
`Appender`, `COPY FROM`, `INSERT ... SELECT`, `TRUNCATE`, multi-chunk DML under
`threads=8`, a user's own trigger coexisting on a tracked table, a double-count
guard that asserts a **sum** rather than a row count, `ALTER TABLE ... ADD
COLUMN` regenerating the bodies, `DROP TABLE` + recreate reinstalling them, DDL
inside an explicit transaction (`ADD COLUMN` with and without a write in the
same transaction, an `ALTER` that rolls back, `CREATE TABLE` + `dbsp_track`
committed and rolled back), a tracked catalog still being `DETACH`able, and an
empty install record re-using the bodies already in the catalog instead of
re-issuing the DDL.

It also pins what tracking a table COSTS it — the statements the engine refuses
on a triggered table (`MERGE INTO`, `ON CONFLICT DO UPDATE`, `INSERT OR
REPLACE`, `ALTER TABLE ... RENAME COLUMN`), and that tracking is REFUSED
outright on a database below storage version v2.0.0, with an error naming the
migration. Those are product constraints now, so they are asserted rather than
discovered. See `docs/DESIGN_TRIGGER_SOURCE.md`.

One case is not about triggers at all but lives here because this is where the
oracle is: `cdc: create_view seeds its source baseline, not an empty one`. The
public `dbsp_track` leaves a baseline empty by design, so a view created
straight afterwards used to be built over nothing unless an unrelated commit
happened to scan-sync the table first — a permanently wrong answer with no
error. Its five sections remove that accident (an earlier FAILED DBSP call
does it) and assert the view matches SQL through two later edits, so a constant
offset cannot hide. `test/python/test_create_view_seeding.py` is the
file-backed sibling.

Three more cases exist because the sweep runs concurrently — with the user, and
with its own past. Bodies lost WITHOUT a fingerprint change (an
in-transaction `DROP`+`CREATE` of the same shape, and a hand-dropped body) must
come back on the next sweep; and a racing creator of `dbsp_trigger_sink`, held
open on a second connection, must not fail the user's statement.

### `dml_shapes` — shapes a delta source can get wrong

`test_dml_shapes` (`test/integration/test_dml_shapes.cpp`, 10 cases) collects
the DML shapes that defeated earlier delta sources: a table written twice in
one transaction, a predicate reading transaction-local state, `UPDATE ... FROM`
(including an ambiguous multi-match), a volatile SET expression, an
indexed-column UPDATE the engine runs as delete+re-append, non-repeatable
INSERT sources (table functions, `USING SAMPLE`, sequence DEFAULTs) and
multi-statement DML in one string. Each checks the view against direct SQL AND
asserts via counters that no scan ran — a fallback would still produce the
right view, so the correctness check alone could not fail on a silent
regression.

### Python scripts (`test/python/`)

`test/python/*.py` are standalone probe scripts, **not wired into ctest** —
they only run when someone runs them. Each takes the extension path as its
one argument and prints `PASS` (exit 0) or fails loudly:

```bash
uv run --isolated --with 'duckdb==1.6.0.dev379' --with pyarrow \
  python test/python/test_ddl_syntax.py build/dbsp.duckdb_extension
```

**Fetch every table function.** `con.execute("SELECT * FROM dbsp_track('t')")`
without a `.fetchall()` does NOTHING: a table function that acts in its execute
callback never runs if its result is not consumed. Measured — `dbsp_tables()`
returns `[]` without the fetch and `[('…main.t', 2)]` with it. `dbsp_create_view`
happens to act in BIND, so it works either way, which is exactly what makes the
trap quiet: a script can look like it tracks and creates, and only the create
actually happened. `test_create_view_seeding.py` was written that way and could
not fail until it was corrected. Fetch, and assert on what the call was supposed
to change.

They exercise what only the loadable extension on a real Python client can
reach: the SQL DDL front door (`test_ddl_syntax.py`), `dbsp_mv_tables`
semantics, window frames, self-joins. Close the connection in any script you
add — an open DBSP connection at interpreter exit SIGSEGVs on the 2.0 alpha
(CHANGELOG, "DuckDB 2.0 alpha issues").

`test_trigger_source.py` is the one that has to run here rather than in ctest:
it checks the delta source inside a wheel straight from PyPI, which is the
whole claim of that source and something no in-tree binary can demonstrate. It
also pins the sink bound (`DBSP_TRIGGER_SINK_DRAIN` lowered so 100 statements
suffice) and that an attached catalog holding a triggered table still detaches.

**Known reds, measured 2026-09-04 on `v2.0.0-alpha39998`:** one.
`test_mv_tables.py` (`disable must stop mirroring`), pre-existing and unrelated
to the delta source. 26 of the 27 scripts exit 0.

The exit-139 scripts were never an engine problem to live with: they left a
DBSP connection open at interpreter exit, or exited while a detached teardown
thread was still running. `close()` fixed two outright; two more needed
`close()` plus a `dbsp_wait_teardown()` drain on a fresh connection, which is
the pattern to copy. Close what you open.

Benchmarks and the soak test build alongside but are not part of ctest:

```bash
make bench_planner_eval soak_differential
./bench_planner_eval                        # throughput + RAM benches
SOAK_ROUNDS=60 ./soak_differential "[soak]" # randomized long-run churn
```

Sanitizer builds live in sibling directories with the same CMake setup:

```bash
cd test/build_asan   # RelWithDebInfo + AddressSanitizer
ASAN_OPTIONS=detect_leaks=0 ./test_planner_frontend
# (leak detection off: the CDC manager is a deliberately leaked singleton)

cd test/build_tsan   # ThreadSanitizer
./test_planner_frontend "[parallel],[spill],[shard]"
./test_thread_safety
```

## The layers

### 1. Unit tests (`test/unit/`)

Direct exercises of one component: Z-set algebra and the dense
FlatWeightMap (including property tests against an oracle map with
hostile hash functions), the spill store (row codec round-trips,
rebuild diffs, bucket-index property tests across compactions), native
view classes, CDC manager locking, security validation.

### 2. Integration tests (`test/integration/`)

Everything through the real extension surface: table functions, CDC,
cascades, recovery, persistence, ACID behavior. The centerpiece is
`test_planner_frontend.cpp` — for every supported SQL shape it creates
a view, runs randomized insert/delete rounds (NULLs included by the
generators), and after each round compares the view's contents against
DuckDB executing the same SQL directly:

```
expected:  SELECT * FROM (<view SQL>) ORDER BY ALL     -- DuckDB itself
actual:    SELECT * FROM dbsp_query('<view>') ORDER BY ALL
```

Coverage includes joins of all types, correlated subqueries, grouping
sets, ordered and holistic aggregates, window functions over
expressions, percentage limits, spill-mode variants (bounded top-K
with forced refills, spilled arrangements with live migration), and
parallel propagation.

### 3. Soak test (`test/benchmarks/soak_differential.cpp`)

Stacked views (outer join → aggregate → sort, NOT IN, recursive)
under hundreds of randomized delete-heavy rounds, differentially
checked every round. `SOAK_ROUNDS` scales the run.

### 4. Sanitizers

ASAN and TSAN builds run the same suites. TSAN specifically pins the
concurrency claims: parallel view propagation, sharded join probes,
and concurrent probes of one spilled shared arrangement.

### 5. Benchmarks (`test/benchmarks/bench_planner_eval.cpp`)

Throughput ledgers (filter/aggregate/join rows-per-second, cascade
sync latency, captured-commit latency), spill-mode RAM/CPU trade
(maxrss comparison), arrangement-sharing and sharded-probe contrasts.
Perf-sensitive changes are gated on these staying inside their noise
bands — regressions here have reverted otherwise-working designs.

## Conventions

- Tests encode intent, not just behavior: when a formerly rejected SQL
  construct becomes supported, the test asserting the rejection is
  updated to assert the support (and this shows up in the commit).
- Randomized generators always include NULLs and duplicate values.
- New state machinery ships with a property test against a plain
  in-memory oracle (see `test_zset.cpp`, `test_spill_store.cpp`).
- Bench numbers quoted in commits come from `test/build_test` (release
  flags); sanitizer-build numbers are 20-30x slower and never quoted.

For questions or issues, see [CONTRIBUTING.md](../CONTRIBUTING.md).
