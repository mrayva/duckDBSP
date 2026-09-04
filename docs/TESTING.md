# Testing Guide

How duckDBSP is tested, how to run the suites, and what each layer
covers. The single most important idea: **differential testing** —
after every batch of changes, an incrementally maintained view must
equal DuckDB's own answer for the same SQL. DuckDB is the oracle; the
engine never gets to grade its own homework.

## Running the suites

All tests build in `test/build_test`. ctest registers **46** entries with
the default `-DDBSP_ENGINE_HOOK=OFF`-equivalent tree and **48** with
`-DDBSP_ENGINE_HOOK=ON`: the two extra are `engine_hook` and
`engine_hook_consumer`, which only compile against a patched engine
(`test/CMakeLists.txt`). Two of the 46 are bench binaries registered
as smoke entries (`planner_eval_smoke`, `window_bench`).

```bash
cd test/build_test
cmake .. && make -j8
ctest                       # full suite, ~15-45s
./test_planner_frontend     # the big differential suite on its own
```

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

### `trigger_source` — the trigger delta source

`test_trigger_source` (`test/unit/test_trigger_source.cpp`, registered as an
integration test because it needs the extension) arms
`DBSP_DELTA_SOURCE=trigger` **process-wide** from a static initializer, before
any harness opens a database — the mode is read once and cached, so it cannot
be set later. That is why it is its own binary: no other test may run in that
mode, and this one may not run in any other.

It is deliberately built **without** `DBSP_ENGINE_HOOK`. The claim the suite
exists to check is that trigger-fed deltas need no patched engine, so nothing
in it may be served by the engine callback.

```bash
cd test/build_test
./test_trigger_source                      # 11 cases
DBSP_DELTA_SOURCE=trigger ./test_...       # redundant: the binary sets it
```

It ports the engine-hook differential oracle (old images −1, new +1,
insert-then-delete nets to zero, update chains, rollback, multi-table commits)
and adds the paths specific to this source: the C++ `Appender`, `COPY FROM`, a
double-count guard that asserts a **sum** rather than a row count, and a pin on
`MERGE INTO` being rejected outright on a tracked table. See
`docs/DESIGN_TRIGGER_SOURCE.md`.

### Python scripts (`test/python/`)

`test/python/*.py` are standalone probe scripts, **not wired into ctest** —
they only run when someone runs them. Each takes the extension path as its
one argument and prints `PASS` (exit 0) or fails loudly:

```bash
uv run --isolated --with 'duckdb==1.6.0.dev379' --with pyarrow \
  python test/python/test_ddl_syntax.py build/dbsp.duckdb_extension
```

They exercise what only the loadable extension on a real Python client can
reach: the SQL DDL front door (`test_ddl_syntax.py`), `dbsp_mv_tables`
semantics, window frames, self-joins. Close the connection in any script you
add — an open DBSP connection at interpreter exit SIGSEGVs on the 2.0 alpha
(CHANGELOG, "DuckDB 2.0 alpha issues").

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
