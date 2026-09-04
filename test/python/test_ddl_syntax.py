"""DDL syntax test: CREATE / REFRESH / DROP MATERIALIZED VIEW.

This is the extension's SQL-DDL front door, and NumPad's only route into it
(calcengine/engine/mvcompile rewrites CREATE VIEW -> CREATE MATERIALIZED VIEW).

The statement is recognised by `ParserExtension::parser_override`, which gets
the RAW query text and runs BEFORE the core PEG grammar. So the SELECT body a
view stores is a substring of what the user typed -- comments, spacing and all
-- and `DROP MATERIALIZED VIEW` is reachable, which it was not while the only
hook ran on PEG FAILURES (the 2.0 grammar claims DROP and its transformer
throws `Cannot drop MATERIALIZED VIEW yet`).

The older token-reconstruction path is still there and still parses the same
statements: `parser_override` callbacks are skipped unless
`allow_parser_override_extension` is FALLBACK or STRICT, and the extension
raises it to FALLBACK at load. A database where that setting is put back to
DEFAULT keeps working, with normalised stored SQL and no DROP DDL. Both are
exercised below.

Covered:
  - a quoted, mixed-case identifier survives
  - a string literal with an escaped quote and runs of two spaces survives
    verbatim
  - qualified `t.col` references resolve
  - negative literals survive
  - `--` line comments and block comments inside the SELECT are KEPT
  - `||` concatenation is not split
  - the resulting dbsp_query('v') matches the same SQL run natively
  - the SQL `dbsp_views()` reports is BYTE-EXACT with what was typed
  - REFRESH MATERIALIZED VIEW reports the view is incrementally maintained
  - DROP MATERIALIZED VIEW, with IF EXISTS, with CASCADE, and the refusal
    when a dependent exists
  - dbsp_drop_view still works (NumPad calls it)
  - a multi-statement input errors LOUDLY and does not silently execute or
    silently drop the trailing statement
  - the same DDL under allow_parser_override_extension=DEFAULT still builds a
    correct view through the token path

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

    # BYTE-EXACT: parser_override keeps the raw text, so what dbsp_views()
    # reports is the substring the user typed after AS, trimmed. The token
    # path could not do this -- it returned `t . "MixedCol" AS m , ...` with
    # every comment gone.
    stored = conn.execute(
        "SELECT sql FROM dbsp_views() WHERE view_name = 'v'"
    ).fetchone()[0]
    assert stored == SELECT_BODY.strip(), (
        "stored SQL is not byte-exact:\n"
        f"  got  {stored!r}\n  want {SELECT_BODY.strip()!r}"
    )
    assert "-- quoted mixed-case identifier" in stored, "line comment lost"
    assert "/* a block comment, mid-statement */" in stored, "block comment lost"
    print("ok: stored SQL is byte-exact, comments preserved", flush=True)

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

    # --- DROP MATERIALIZED VIEW is ours again ----------------------------------
    # It was NOT reachable while the only hook ran on PEG failures: DuckDB 2.0
    # added `MaterializedViewEntry <- 'MATERIALIZED' 'VIEW'` to its own drop
    # grammar (duckdb/src/parser/peg/grammar/statements/drop.gram:32), so the
    # parse SUCCEEDED and the core transformer threw
    # `NotImplementedException: Cannot drop MATERIALIZED VIEW yet`
    # (duckdb/src/parser/peg/transformer/transform_drop.cpp:34). parser_override
    # runs BEFORE that grammar, so the statement never reaches it.
    conn.execute("CREATE MATERIALIZED VIEW dropme AS SELECT count(*) AS n FROM t")
    assert conn.execute(
        "SELECT count(*) FROM dbsp_views() WHERE view_name = 'dropme'"
    ).fetchone()[0] == 1
    conn.execute("DROP MATERIALIZED VIEW dropme")
    assert conn.execute(
        "SELECT count(*) FROM dbsp_views() WHERE view_name = 'dropme'"
    ).fetchone()[0] == 0, "DROP MATERIALIZED VIEW did not drop the view"
    print("ok: DROP MATERIALIZED VIEW drops the view", flush=True)

    # Without IF EXISTS a missing view is an error; with it, a message.
    try:
        conn.execute("DROP MATERIALIZED VIEW nosuchview")
        raise AssertionError("DROP of a missing view should have raised")
    except duckdb.InvalidInputException as e:
        assert "does not exist" in str(e), f"unexpected error: {e}"
    msg = conn.execute("DROP MATERIALIZED VIEW IF EXISTS nosuchview").fetchall()
    assert len(msg) == 1 and "IF EXISTS" in msg[0][0], f"unexpected result: {msg}"
    print("ok: DROP MATERIALIZED VIEW honours IF EXISTS", flush=True)

    # A view with a dependent is refused, and CASCADE takes both -- INCLUDING
    # the named view itself. get_drop_order returns the dependents only, so
    # the cascade branch used to leave the named view behind; the first run of
    # this path reported "a1 (and 1 dependent views)" with a1 still listed.
    conn.execute(
        "CREATE MATERIALIZED VIEW casc1 AS SELECT tag, count(*) AS n FROM t GROUP BY tag")
    conn.execute("CREATE MATERIALIZED VIEW casc2 AS SELECT tag FROM casc1")
    try:
        conn.execute("DROP MATERIALIZED VIEW casc1")
        raise AssertionError("DROP of a view with a dependent should have raised")
    except duckdb.InvalidInputException as e:
        assert "casc2" in str(e), f"error should name the dependent: {e}"
    conn.execute("DROP MATERIALIZED VIEW casc1 CASCADE")
    left = [r[0] for r in conn.execute(
        "SELECT view_name FROM dbsp_views() WHERE view_name IN ('casc1','casc2')"
    ).fetchall()]
    assert left == [], f"CASCADE left views behind: {left}"
    print("ok: DROP MATERIALIZED VIEW CASCADE takes the view and its dependents",
          flush=True)

    # dbsp_drop_view still works: NumPad calls it (calcengine/session/mv_reattach.py).
    conn.execute("CREATE MATERIALIZED VIEW fn_drop AS SELECT count(*) AS n FROM t")
    assert conn.execute("SELECT dbsp_drop_view('fn_drop')").fetchone()[0] == "Dropped"
    assert conn.execute(
        "SELECT count(*) FROM dbsp_views() WHERE view_name = 'fn_drop'"
    ).fetchone()[0] == 0
    print("ok: dbsp_drop_view still works", flush=True)

    # The view under test must be untouched by all of the above.
    got = conn.execute("SELECT * FROM dbsp_query('v') ORDER BY m").fetchall()
    assert got == want, f"view damaged by the DROP work: {got}"
    print("ok: view intact after the DROP cases", flush=True)

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

    # --- the token path is still a working fallback ----------------------------
    # parser_override callbacks are skipped when allow_parser_override_extension
    # is DEFAULT, which is DuckDB's own default -- the extension raises it to
    # FALLBACK at load. Put it back and the DDL still builds a correct view
    # through the token reconstruction; only the stored text is normalised, and
    # DROP MATERIALIZED VIEW goes back to the core parser's refusal.
    assert conn.execute(
        "SELECT current_setting('allow_parser_override_extension')"
    ).fetchone()[0] == "FALLBACK", "the extension did not raise the setting at load"
    conn.execute("SET allow_parser_override_extension='DEFAULT'")
    conn.execute("CREATE MATERIALIZED VIEW tokpath AS SELECT tag, count(*) AS n "
                 "FROM t GROUP BY tag")
    got = sorted(conn.execute("SELECT * FROM dbsp_query('tokpath')").fetchall())
    want_tok = sorted(conn.execute(
        "SELECT tag, count(*) AS n FROM t GROUP BY tag").fetchall())
    assert got == want_tok, f"token path built a wrong view: {got} vs {want_tok}"
    try:
        conn.execute("DROP MATERIALIZED VIEW tokpath")
        raise AssertionError(
            "DROP MATERIALIZED VIEW worked with overrides off -- the core parser "
            "must own it there")
    except duckdb.NotImplementedException as e:
        assert "Cannot drop MATERIALIZED VIEW" in str(e), f"unexpected error: {e}"
    conn.execute("SET allow_parser_override_extension='FALLBACK'")
    print("ok: token path still builds a correct view with overrides off",
          flush=True)
finally:
    conn.close()

print("PASS", flush=True)
