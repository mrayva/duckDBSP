"""DDL syntax test: CREATE / REFRESH / DROP MATERIALIZED VIEW.

This is the extension's SQL-DDL front door, and NumPad's only route into it
(calcengine/engine/mvcompile rewrites CREATE VIEW -> CREATE MATERIALIZED VIEW).
DuckDB 2.0 replaced the PostgreSQL-derived parser with a PEG parser and changed
the parser-extension contract with it: the hook no longer receives the raw
statement text, it receives the tokenized tail from the PEG failure point, and
include/dbsp_parser_extension.hpp RECONSTRUCTS the statement by joining token
slices with single spaces. Everything below exists to pin that reconstruction,
because a lossy rebuild would silently change what the view computes.

Covered:
  - a quoted, mixed-case identifier survives (quotes are part of the slice)
  - a string literal with an escaped quote and runs of two spaces survives
    verbatim -- the single-space join must not touch the inside of a token
  - qualified `t.col` references still resolve when rebuilt as `t . col`
  - negative literals survive the `-` / `1` and `-` / `10` token splits
  - `--` line comments and block comments inside the SELECT are dropped
  - `||` concatenation is not split
  - the resulting dbsp_query('v') matches the same SQL run natively
  - REFRESH MATERIALIZED VIEW reports the view is incrementally maintained
  - DROP MATERIALIZED VIEW: never reaches the extension (also true on
    1.5.4); pinned so the day it changes, this test says so
  - a multi-statement input errors LOUDLY and does not silently execute or
    silently drop the trailing statement

Run: python test_ddl_syntax.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import sys

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 60


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)

conn = duckdb.connect(config={"allow_unsigned_extensions": "true"})
conn.execute(f"LOAD '{EXT}'")

# Everything below runs inside try/finally so the connection is CLOSED even
# when an assertion fires. The 2.0 alpha SIGSEGVs at interpreter exit if an
# instance holding DBSP views is destroyed during static destruction (see
# CHANGELOG, "DuckDB 2.0 alpha issues"), and an exit-139 on the way out would
# replace the assertion message with a bare crash.
try:

    conn.execute('CREATE TABLE t ("MixedCol" INTEGER, tag VARCHAR)')
    conn.execute("INSERT INTO t VALUES (5, 'a'), (-3, 'b'), (7, 'a')")

    # Every hazard of the token->text rebuild in one statement. The literal holds
    # TWO spaces between "two" and "spaces" and an escaped quote; if the rebuild
    # ever tokenized inside a literal, the collapsed spacing would show up in the
    # comparison against native SQL below.
    SELECT_BODY = """
    SELECT t."MixedCol" AS m,                        -- quoted mixed-case identifier
           t.tag || '-' || 'it''s  two  spaces' AS lbl,
           /* a block comment, mid-statement */
           -1 AS neg
    FROM t
    WHERE t."MixedCol" > -10
    """

    conn.execute(f"CREATE MATERIALIZED VIEW v AS {SELECT_BODY}")
    print("ok: CREATE MATERIALIZED VIEW accepted", flush=True)

    got = conn.execute("SELECT * FROM dbsp_query('v') ORDER BY m").fetchall()
    want = conn.execute(f"{SELECT_BODY} ORDER BY m").fetchall()
    assert got == want, f"materialized view != native SQL:\n  got  {got}\n  want {want}"
    assert got == [
        (-3, "b-it's  two  spaces", -1),
        (5, "a-it's  two  spaces", -1),
        (7, "a-it's  two  spaces", -1),
    ], f"unexpected rows (literal spacing or negative literal mangled?): {got}"
    print("ok: dbsp_query matches native SQL, literal and comments intact", flush=True)

    # The view is a real materialized view, not a snapshot: it must track writes.
    conn.execute("INSERT INTO t VALUES (11, 'c')")
    got = conn.execute("SELECT * FROM dbsp_query('v') ORDER BY m").fetchall()
    want = conn.execute(f"{SELECT_BODY} ORDER BY m").fetchall()
    assert got == want, f"view went stale after INSERT:\n  got  {got}\n  want {want}"
    print("ok: view stays incrementally correct after a write", flush=True)

    # REFRESH is accepted and reports that no manual refresh is needed.
    msg = conn.execute("REFRESH MATERIALIZED VIEW v").fetchall()
    assert len(msg) == 1 and "up-to-date" in msg[0][0], f"unexpected REFRESH result: {msg}"
    print("ok: REFRESH MATERIALIZED VIEW reports incremental maintenance", flush=True)

    # --- PINNED: DROP MATERIALIZED VIEW never reaches the extension -------------
    # NOT a 2.0 regression -- MEASURED on the 1.5.4 stack (build_v1.5.4 extension
    # under duckdb==1.5.4): `DROP MATERIALIZED VIEW v` and the IF EXISTS form both
    # raise `NotImplementedException: Not implemented Error: Cannot drop this type
    # yet`, and `SELECT dbsp_drop_view('v')` returns 'Dropped'. So DuckDB has owned
    # this statement since at least 1.5.4 and the extension's
    # ParseDropMaterializedView has never been reachable through it; docs/API.md
    # has said so since 2026-07-05.
    # What 2.0 changed is only the message and the mechanism: 2.0 added
    # `MaterializedViewEntry <- 'MATERIALIZED' 'VIEW'` to its OWN drop grammar
    # (duckdb/src/parser/peg/grammar/statements/drop.gram:32), so the PEG parse
    # SUCCEEDS and the core transformer throws:
    #   duckdb/src/parser/peg/transformer/transform_drop.cpp:34
    #     throw NotImplementedException("Cannot drop MATERIALIZED VIEW yet");
    # This assertion pins that state on purpose: when upstream implements DROP
    # MATERIALIZED VIEW (or the fork reclaims the statement via
    # ParserExtension::parser_override), this test fails and whoever is here can
    # re-point callers. NumPad used to issue `DROP MATERIALIZED VIEW IF EXISTS`
    # from calcengine/session/mv_reattach.py and now calls dbsp_drop_view.
    for stmt in ("DROP MATERIALIZED VIEW v", "DROP MATERIALIZED VIEW IF EXISTS v"):
        try:
            conn.execute(stmt)
            raise AssertionError(
                f"{stmt} unexpectedly SUCCEEDED -- the 2.0 grammar regression is "
                "fixed; re-route the fork's DROP path and update this test"
            )
        except duckdb.NotImplementedException as e:
            assert "Cannot drop MATERIALIZED VIEW" in str(e), f"{stmt}: unexpected error {e}"
    print(
        "ok: DROP MATERIALIZED VIEW blocked by the core parser, as on 1.5.4 "
        "(pinned; use dbsp_drop_view)",
        flush=True,
    )

    # The view must survive a failed DROP rather than being half-torn-down.
    got = conn.execute("SELECT * FROM dbsp_query('v') ORDER BY m").fetchall()
    assert got == want, f"view damaged by the failed DROP: {got}"
    print("ok: view intact after the failed DROP", flush=True)

    # --- multi-statement input must not be silently truncated ------------------
    # The hook claims the whole token tail, terminator included, so a trailing
    # statement would be folded into the SELECT body rather than executed. That
    # must surface as an error, never as a silently dropped statement.
    before = conn.execute("SELECT count(*) FROM t").fetchone()[0]
    try:
        conn.execute(
            "CREATE MATERIALIZED VIEW mv2 AS SELECT count(*) c FROM t; "
            "INSERT INTO t VALUES (99, 'z')"
        )
        raise AssertionError(
            "multi-statement CREATE MATERIALIZED VIEW was accepted -- check whether "
            "the trailing INSERT ran or was silently dropped"
        )
    except duckdb.Error as e:
        assert "mv2" in str(e) or "syntax error" in str(e), f"unexpected error: {e}"
    after = conn.execute("SELECT count(*) FROM t").fetchone()[0]
    assert after == before, f"the trailing INSERT ran anyway: {before} -> {after} rows"
    mv2 = conn.execute(
        "SELECT count(*) FROM dbsp_views() WHERE view_name = 'mv2'"
    ).fetchone()[0]
    assert mv2 == 0, "mv2 was created despite the error"
    print("ok: multi-statement input errors loudly, nothing half-applied", flush=True)
finally:
    conn.close()

print("PASS", flush=True)
