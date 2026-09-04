# Known limitations & candidate work

Accurate as of 2026-07-05 (post Phase C + pre-Phase-D hardening; history in
CHANGELOG.md). The February 2026 code-review dump that used to live here is
gone — everything actionable from it was either fixed (thread safety, SQL
injection, debug prints, singleton races, path traversal, read isolation)
or became obsolete when the code it referenced was deleted (checkpoint/WAL
subsystem, bespoke parser, standalone Z-set spilling).

## Not supported (DBSP-E110 at view creation)

- WITH RECURSIVE ... USING KEY
- Non-constant (expression) LIMIT — constant and percentage forms work
- approx_quantile / reservoir_quantile / approx_top_k (approximate
  results can't match DuckDB in differentials; exact mapping would
  silently differ)
- DISTINCT on holistic aggregates (median/quantile/mode)
- MODE tie-breaking differs from DuckDB on ties (ours: smallest value;
  DuckDB: scan-order-dependent) — unreproducible incrementally
- string_agg/array_agg WITHOUT ORDER BY inside the aggregate (result
  order unreproducible incrementally; the ordered forms are supported —
  ties on order keys break by value, not input order)
- NTH_VALUE's N — rejected even as a literal constant (e.g. `NTH_VALUE(v,
  3)`), unlike window frame bounds, LAG/LEAD offsets, and (as of the
  2026-07-31 lazy-restore-ntile fix) NTILE's bucket count, all of which
  now accept constants. Deliberately kept gated: `NativeWindowView`'s
  NTH_VALUE render logic (both call sites in
  `include/dbsp_window_view.hpp`) always indexes the N-th row of the
  whole partition, ignoring the window's frame bounds — diverges from
  stock DuckDB's frame-relative NTH_VALUE whenever the frame is narrower
  than the full partition (the common case, since the default frame is
  `RANGE UNBOUNDED PRECEDING AND CURRENT ROW`), see CHANGELOG. Fix
  NTH_VALUE to index within `[frame_start, frame_end]` like the aggregate
  branch beside it does, then widen the gate (`bare_constant_int` ->
  `constant_int` at its one remaining call site in
  `dbsp_plan_translator.hpp`).

## Performance

- **O(Δ) sync covers every write to a tracked table.** `dbsp_track` puts
  statement-level AFTER triggers on the table; their bodies hand the exact old
  and new row images to the extension as the statement runs, so INSERT (VALUES,
  SELECT, COPY FROM, the C++ Appender), UPDATE, DELETE and TRUNCATE all commit
  in O(Δ), whatever the predicate — including shapes nothing could predict
  (UPDATE...FROM, volatile expressions, prepared parameters, post-write
  subqueries, indexed-column UPDATEs, same-table-twice transactions,
  multi-statement DML strings). Scan-and-diff, scoped to the tables the
  transaction wrote, is the fallback for what the triggers could not account
  for: a write matching zero rows, a transaction under which the triggers were
  installed or regenerated, an unparseable statement, a conversion failure.
  The price is what the engine then refuses on a tracked table — MERGE INTO,
  ON CONFLICT DO UPDATE, INSERT OR REPLACE, every ALTER but ADD COLUMN — and a
  storage version of v2.0.0 or higher (docs/DESIGN_TRIGGER_SOURCE.md).
  Engine-behavior assumptions are pinned
  by test/integration/test_engine_assumptions.cpp — run it FIRST on any
  engine bump.
- Phase D1 vectorized filter/map/fused evaluation + zero-copy circuit
  deltas: fused filter 259k→644k rows/s, aggregate 770k→1.88M, join delta
  140k→265k. The Z-set ingestion hash cost is now FIXED (DP1 vectorized
  row hashing, 2.25× ingestion; docs/DESIGN_DATA_PLANE.md). Remaining
  data-plane phases (batched key eval, late materialization, columnar
  state) are scoped there, workload-gated.
- Aggregate keys/args, join keys, and residuals still evaluate per-row
  (RowExprEval); batch if profiles demand.
- Row-hash caching: DONE (G1, ColumnVec; H3 dense-map storage on top).
- Compact row encoding: SUPERSEDED by H6' copy-on-write payloads (row
  copies are refcount bumps now). Byte encoding would additionally need
  order-preserving encodings for every typed comparator — revisit only if
  profiles show payload allocation itself dominating.
- Join residual predicates still evaluate per candidate pair (RowExprEval);
  batch only if residual-heavy joins show up in profiles.
- Shared join arrangements: self-padding sides (right of RIGHT/FULL,
  left of LEFT/FULL/MARK) stay private because init-replay skip would
  lose their unmatched-row pads. Fingerprints include the side's column
  projection, so views needing different column subsets of the same
  table do not share (a maximal-columns arrangement + per-consumer
  projection could lift this).
- UNION ALL recursive deletion still triggers a full fixed-point recompute
  (multiplicity-in-cycles is ill-defined; UNION recursion is incremental
  via DRed).

- **Engine-hook fork proposal** (docs/DESIGN_ENGINE_HOOK.md): a single
  ~300-line patch to the pinned engine (commit-time modification
  callback walking the UndoBuffer) would replace the entire capture
  stack with exact per-commit deltas — 100% write coverage incl.
  Appender, ~0.3ms per statement, guards deleted, upgrade cost collapsed
  to one rebaseable commit. Unscheduled; dual-mode design keeps
  official-build hosts on the shipped stack.

## Architectural

- Spill mode (K1+K2+N2-N4) covers baselines, shared arrangements,
  local probe-target join indexes, bounded top-K sort views (constant
  AND percentage limits — the percentage cutoff now rides a scalar
  total-count with a dynamic window cap; the overflow-log refill absorbs
  cutoff growth), and oversized holistic groups. Still RAM-resident:
  self-padding join sides (pad/weight/mark reconciliation walks full-row
  structures), mode value-counts, ordered-aggregate (string_agg)
  entries, and plain ORDER BY / window views — for those the answer IS
  the total order: the result Z-set the view interface returns by
  reference is the memory floor, and row payloads are already COW-shared
  with upstream state, so a payload-spill would save little beyond node
  overhead. A known residual: sorted multisets duplicate entries per
  weight unit (weight w = w nodes, shared payload) — a weight-collapsed
  ordered map is the cheap fix if a weighty workload shows up.
- Cross-projection arrangement sharing shipped (O4): full-row
  arrangements + canonical key fingerprints + probe-time consumer
  projection; bench-gated (no regression). Fingerprints no longer
  include projections.

- CDCManager is a deliberately leaked process-wide singleton (views pin
  the DatabaseInstance; instance-scoped ownership would be a reference
  cycle). Multi-database processes share one manager.
