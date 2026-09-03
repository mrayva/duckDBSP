# Testing Guide

How duckDBSP is tested, how to run the suites, and what each layer
covers. The single most important idea: **differential testing** —
after every batch of changes, an incrementally maintained view must
equal DuckDB's own answer for the same SQL. DuckDB is the oracle; the
engine never gets to grade its own homework.

## Running the suites

All tests build in `test/build_test`. ctest registers **45** entries with
the default `-DDBSP_ENGINE_HOOK=OFF`-equivalent tree and **47** with
`-DDBSP_ENGINE_HOOK=ON`: the two extra are `engine_hook` and
`engine_hook_consumer`, which only compile against a patched engine
(`test/CMakeLists.txt:86,106`). Two of the 45 are bench binaries registered
as smoke entries (`planner_eval_smoke`, `window_bench`).

```bash
cd test/build_test
cmake .. && make -j8
ctest                       # full suite, ~15-45s
./test_planner_frontend     # the big differential suite on its own
```

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
