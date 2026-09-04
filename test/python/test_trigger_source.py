"""Trigger delta source on a real Python client, on a STOCK engine.

`DBSP_DELTA_SOURCE=trigger` (docs/DESIGN_TRIGGER_SOURCE.md) makes dbsp_track
generate statement-level AFTER triggers whose bodies feed the row images to the
extension. The C++ suite (test/unit/test_trigger_source.cpp) proves the oracle;
this proves the same thing where it actually has to hold — a loadable extension
inside an UNPATCHED DuckDB wheel from PyPI, which is the whole point of this
source. Nothing here can be served by the engine-hook callback, because the
wheel has no such callback.

Covered:
  - dbsp_stats() reports delta_source_mode = 3, so a mis-set variable is loud
  - the three triggers exist in duckdb_triggers() after tracking
  - dbsp_query equals plain SQL after INSERT, UPDATE and DELETE
  - trigger_syncs / trigger_rows climb while capture_guard_fallbacks does not,
    and the commits are served by exact deltas rather than scans
  - autopersist close-and-reopen re-installs the triggers (CREATE OR REPLACE
    must not fail on "trigger already exists") and the view stays correct

Run: DBSP_DELTA_SOURCE=trigger python test_trigger_source.py <path-to-ext>
Without the variable the script sets it itself and re-executes, because the
mode is read ONCE at extension load and cannot be changed afterwards.
"""

import os
import pathlib
import signal
import subprocess
import sys
import tempfile

# The child phase below runs with the variable DELIBERATELY unset, so it must
# be handled before the re-exec that arms trigger mode for everything else.
CHILD_FLAG = "--default-mode-child"
IS_CHILD = len(sys.argv) > 1 and sys.argv[1] == CHILD_FLAG

if not IS_CHILD and os.environ.get("DBSP_DELTA_SOURCE") != "trigger":
    os.environ["DBSP_DELTA_SOURCE"] = "trigger"
    os.execv(sys.executable, [sys.executable] + sys.argv)

import duckdb  # noqa: E402  (must be imported with the variable already set)

EXT = (sys.argv[2] if IS_CHILD else
       (sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"))
TIMEOUT_S = 120


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)


def connect(path):
    con = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")
    return con


def stats(con):
    return dict(con.execute("SELECT * FROM dbsp_stats()").fetchall())


# ---- child phase: the same database, opened WITHOUT the variable ------------
# Triggers are catalog objects and outlive the mode that created them. Opening
# such a database in any other mode must NOT have the persisted bodies deliver
# alongside that mode's own source — measured before the guard as
# `delta_source_mode 0` with `trigger_syncs 3`, i.e. every row counted twice on
# a hook build.
if IS_CHILD:
    db_path = sys.argv[3]
    conn = connect(db_path)
    try:
        s = stats(conn)
        assert s["delta_source_mode"] != 3, "child still in trigger mode"
        assert s["trigger_syncs"] == 0, (
            f"persisted triggers delivered in mode {s['delta_source_mode']}: "
            f"trigger_syncs={s['trigger_syncs']} — double counting"
        )
        conn.execute("INSERT INTO t VALUES (3, 11.0)")
        s2 = stats(conn)
        assert s2["trigger_syncs"] == 0, (
            f"persisted triggers delivered on a write in mode "
            f"{s2['delta_source_mode']}: trigger_syncs={s2['trigger_syncs']}"
        )
        got = conn.execute("SELECT * FROM dbsp_query('tot')").fetchall()[0][0]
        want = conn.execute("SELECT SUM(v) FROM t").fetchone()[0]
        assert got == want, f"default-mode view {got} != SQL {want}"
        print(
            f"ok: mode {s2['delta_source_mode']} ignored the persisted triggers "
            f"(trigger_syncs=0) and the view is correct: {got}",
            flush=True,
        )

        # The bodies keep writing one sink row per statement even though the
        # scalar refuses to deliver from them, so the drain must NOT be gated
        # on the mode. DBSP_TRIGGER_SINK_DRAIN (set by the parent) makes the
        # bound observable without running fifty thousand statements.
        for i in range(100):
            conn.execute(f"INSERT INTO t VALUES ({100 + i}, 1.0)")
        rows = conn.execute("SELECT count(*) FROM dbsp_trigger_sink").fetchone()[0]
        interval = int(os.environ.get("DBSP_TRIGGER_SINK_DRAIN", "50000"))
        assert rows <= 2 * interval, (
            f"sink grew unbounded in mode {s2['delta_source_mode']}: "
            f"{rows} rows after 100 statements (drain every {interval})"
        )
        got = conn.execute("SELECT * FROM dbsp_query('tot')").fetchall()[0][0]
        want = conn.execute("SELECT SUM(v) FROM t").fetchone()[0]
        assert got == want, f"default-mode view {got} != SQL {want}"
        print(f"ok: sink bounded at {rows} rows after 100 statements "
              f"(drain every {interval}), view still correct", flush=True)
    finally:
        conn.close()
    print("CHILD PASS", flush=True)
    sys.exit(0)

# Every connection is closed in a finally: the 2.0 alpha SIGSEGVs at
# interpreter exit if an instance holding DBSP views is destroyed during static
# destruction (CHANGELOG, "DuckDB 2.0 alpha issues").
conn = connect(":memory:")
try:
    s0 = stats(conn)
    assert s0["delta_source_mode"] == 3, (
        f"not in trigger mode: delta_source_mode={s0['delta_source_mode']}"
    )
    print("ok: delta_source_mode = 3 (trigger)", flush=True)

    conn.execute("CREATE TABLE items (id INTEGER, name VARCHAR, price DOUBLE)")
    conn.execute("SELECT * FROM dbsp_track('items')")
    conn.execute(
        "SELECT * FROM dbsp_create_view('expensive',"
        " 'SELECT * FROM items WHERE price > 50')"
    )
    conn.execute(
        "SELECT * FROM dbsp_create_view('total', 'SELECT SUM(price) AS s FROM items')"
    )

    triggers = [
        r[0]
        for r in conn.execute(
            "SELECT trigger_name FROM duckdb_triggers() ORDER BY 1"
        ).fetchall()
    ]
    assert triggers == [
        "dbsp_trg_items_del",
        "dbsp_trg_items_ins",
        "dbsp_trg_items_upd",
    ], f"unexpected triggers: {triggers}"
    print("ok: three triggers installed on the tracked table", flush=True)

    def same_as_sql(step):
        got = conn.execute("SELECT * FROM dbsp_query('total')").fetchall()[0][0]
        want = conn.execute("SELECT SUM(price) FROM items").fetchone()[0]
        assert got == want, f"{step}: view {got} != SQL {want}"
        got_n = conn.execute("SELECT count(*) FROM dbsp_query('expensive')").fetchone()[0]
        want_n = conn.execute(
            "SELECT count(*) FROM items WHERE price > 50"
        ).fetchone()[0]
        assert got_n == want_n, f"{step}: view {got_n} rows != SQL {want_n}"
        print(f"ok: {step} — dbsp_query matches plain SQL", flush=True)

    conn.execute("INSERT INTO items VALUES (1,'widget',10.0),(2,'gadget',100.0)")
    same_as_sql("after INSERT")
    conn.execute("UPDATE items SET price = 60.0 WHERE id = 1")
    same_as_sql("after UPDATE")
    conn.execute("DELETE FROM items WHERE id = 2")
    same_as_sql("after DELETE")

    s1 = stats(conn)
    assert s1["trigger_syncs"] > 0, "no trigger body ever delivered"
    assert s1["trigger_rows"] > 0, "no row images buffered"
    assert s1["capture_guard_fallbacks"] == s0["capture_guard_fallbacks"], (
        "the capture stack ran too — that is a double-delivery risk"
    )
    assert s1["captured_delta_syncs"] > 0, "no commit was served by an exact delta"
    print(
        f"ok: trigger_syncs={s1['trigger_syncs']} rows={s1['trigger_rows']} "
        f"exact_syncs={s1['captured_delta_syncs']} scans={s1['scan_syncs']}",
        flush=True,
    )
finally:
    conn.close()

# ---- autopersist: the triggers must come back on reopen ---------------------
with tempfile.TemporaryDirectory() as tmp:
    db_path = str(pathlib.Path(tmp) / "trigger_reopen.duckdb")

    conn = connect(db_path)
    try:
        conn.execute("SELECT * FROM dbsp_autopersist(true)")
        conn.execute("CREATE TABLE t (id INTEGER, v DOUBLE)")
        conn.execute("SELECT * FROM dbsp_track('t')")
        conn.execute("SELECT * FROM dbsp_create_view('tot', 'SELECT SUM(v) AS s FROM t')")
        conn.execute("INSERT INTO t VALUES (1, 5.0)")
        assert conn.execute("SELECT * FROM dbsp_query('tot')").fetchall()[0][0] == 5.0
        conn.execute("SELECT * FROM dbsp_save()")
    finally:
        conn.close()

    conn = connect(db_path)
    try:
        triggers = [
            r[0]
            for r in conn.execute(
                "SELECT trigger_name FROM duckdb_triggers() "
                "WHERE table_name = 't' ORDER BY 1"
            ).fetchall()
        ]
        assert triggers == ["dbsp_trg_t_del", "dbsp_trg_t_ins", "dbsp_trg_t_upd"], (
            f"triggers did not survive reopen: {triggers}"
        )
        conn.execute("INSERT INTO t VALUES (2, 7.0)")
        got = conn.execute("SELECT * FROM dbsp_query('tot')").fetchall()[0][0]
        want = conn.execute("SELECT SUM(v) FROM t").fetchone()[0]
        assert got == want, f"after reopen: view {got} != SQL {want}"
        print("ok: reopen re-installs the triggers and the view stays correct",
              flush=True)
    finally:
        conn.close()

    # Same database, a child interpreter, no DBSP_DELTA_SOURCE at all.
    child_env = {k: v for k, v in os.environ.items()
                 if k != "DBSP_DELTA_SOURCE"}
    child_env["DBSP_TRIGGER_SINK_DRAIN"] = "10"
    proc = subprocess.run(
        [sys.executable, os.path.abspath(__file__), CHILD_FLAG, EXT, db_path],
        env=child_env, capture_output=True, text=True, timeout=TIMEOUT_S,
    )
    sys.stdout.write(proc.stdout)
    if proc.returncode != 0 or "CHILD PASS" not in proc.stdout:
        sys.stderr.write(proc.stderr)
        raise AssertionError("default-mode child failed — see output above")

print("PASS", flush=True)
