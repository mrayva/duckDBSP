# Testing Guide

How duckDBSP is tested, how to run the suites, and what each layer
covers. The single most important idea: **differential testing** —
after every batch of changes, an incrementally maintained view must
equal DuckDB's own answer for the same SQL. DuckDB is the oracle; the
engine never gets to grade its own homework.

## Running the suites

The `duckdb/` submodule tracks current DuckDB main (bumped regularly), and
`DBSP_TIP_PORT`/`DBSP_ENGINE_HOOK` default to the matching configuration
(`ON`/`OFF`), so the plain default build already targets tip:

```bash
cmake -S . -B build -DDUCKDB_SOURCE_DIR="$PWD/duckdb"
cmake --build build -j8
ctest --test-dir build --output-on-failure -j1 --timeout 120
build/test/test_planner_frontend # the big differential suite on its own
```

The validated baseline (last checked against DuckDB main @ `fabf1d60b`,
2026-08-05) is **39/39 CTest binaries passing** (5.48M+ assertions) and
**89/89 planner cases passing** in `test_planner_frontend`. Legacy tests
that directly require the removed write-capture, optimizer plan-tee, or
engine-hook APIs are excluded by default (not built — see the
`if(NOT DBSP_TIP_PORT)` guards in `test/CMakeLists.txt`); this is an
intentional compatibility boundary, not a silent pass. Two correlated
scalar-subquery cases in `test_planner_frontend` are `#ifdef
DBSP_TIP_PORT`-gated to assert a known, tracked decline instead of success
— see TODO.md's "join projection maps" entry.

Tip mode uses scan-and-diff for UPDATE/DELETE paths whose old write-capture
API is unavailable. Holistic aggregate values remain in memory on tip while
table, join, and top-K spill paths remain enabled and tested.

### Legacy patched-engine build (unmaintained)

Only relevant if you have your own patched, pre-tip DuckDB checkout for the
SaaS-fork engine-hook consumer
(`patches/v1.5.4-dbsp-txn-callback.patch`) — the submodule itself no longer
supports this path, since the patch predates the DuckDB API moves tip mode
exists to track:

```bash
cmake -S . -B build-legacy \
  -DDUCKDB_SOURCE_DIR=/path/to/patched/duckdb \
  -DDBSP_TIP_PORT=OFF \
  -DDBSP_ENGINE_HOOK=ON
cmake --build build-legacy -j4
ctest --test-dir build-legacy --output-on-failure
```

The tip port retains the circuit-state checkpoint format from upstream. The
fork WAL improvement is available as an optional logical-delta journal; WAL
serialization uses DuckDB's native value serializer and is flushed after
successful captured-delta application. Recovery still treats committed
DuckDB storage and circuit checkpoints as authoritative, rather than applying
an independent sink snapshot on top of rebuilt state.

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
- Bench numbers quoted in commits come from `build` (release
  flags); sanitizer-build numbers are 20-30x slower and never quoted.

For questions or issues, see [CONTRIBUTING.md](../CONTRIBUTING.md).
