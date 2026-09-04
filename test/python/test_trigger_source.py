"""Trigger delta source on a real Python client, on a STOCK engine.

`dbsp_track` generates statement-level AFTER triggers whose bodies feed the row
images to the extension (docs/DESIGN_TRIGGER_SOURCE.md). The C++ suite
(test/unit/test_trigger_source.cpp) proves the oracle; this proves the same
thing where it actually has to hold — a loadable extension inside an UNPATCHED
DuckDB wheel from PyPI, which is the whole point of this source.

Covered:
  - the three triggers exist in duckdb_triggers() after tracking, and the sink
    table is created in the tracked table's own catalog
  - dbsp_query equals plain SQL after INSERT, UPDATE and DELETE
  - trigger_syncs / trigger_rows climb and the commits are served by exact
    deltas rather than scans
  - dbsp_stats() carries no retired hook/capture counters
  - the sink the bodies write into stays bounded (the drain interval is
    overridden so the bound is observable without 50,000 statements)
  - autopersist close-and-reopen re-installs the triggers (CREATE OR REPLACE
    must not fail on "trigger already exists") and the view stays correct
  - a catalog holding a tracked, triggered table can still be DETACHed

Run: python test_trigger_source.py <path-to-ext>
"""

import os
import pathlib
import signal
import sys
import tempfile

# Read once at first use inside the extension, so it must be set before any
# connection commits anything.
os.environ.setdefault("DBSP_TRIGGER_SINK_DRAIN", "10")

import duckdb  # noqa: E402

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
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


# Every connection is closed in a finally: the 2.0 alpha SIGSEGVs at
# interpreter exit if an instance holding DBSP views is destroyed during static
# destruction (CHANGELOG, "DuckDB 2.0 alpha issues").
conn = connect(":memory:")
try:
    s0 = stats(conn)
    retired = {"delta_source_mode", "capture_guard_fallbacks"} & set(s0)
    assert not retired, f"retired counters are back in dbsp_stats(): {retired}"
    assert "trigger_syncs" in s0 and "trigger_rows" in s0, (
        f"dbsp_stats() lost the trigger counters: {sorted(s0)}"
    )
    print("ok: dbsp_stats() reports the trigger counters and nothing retired",
          flush=True)

    conn.execute("CREATE TABLE items (id INTEGER, name VARCHAR, price DOUBLE)")
    conn.execute("SELECT * FROM dbsp_track('items')")
    conn.execute(
        "SELECT * FROM dbsp_create_view('expensive',"
        " 'SELECT * FROM items WHERE price > 50')"
    )
    conn.execute(
        "SELECT * FROM dbsp_create_view('total', 'SELECT SUM(price) AS s FROM items')"
    )

    # Names carry a hash of the table key and the column fingerprint, so they
    # are matched by shape rather than spelled out.
    triggers = sorted(
        r[0]
        for r in conn.execute(
            "SELECT trigger_name FROM duckdb_triggers()"
        ).fetchall()
    )
    assert len(triggers) == 3 and all(
        t.startswith("dbsp_trg_items_") for t in triggers
    ), f"unexpected triggers: {triggers}"
    assert sorted(t.rsplit("_", 1)[1] for t in triggers) == ["del", "ins", "upd"], (
        f"unexpected trigger ops: {triggers}"
    )
    sinks = conn.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = 'dbsp_trigger_sink'"
    ).fetchone()[0]
    assert sinks == 1, f"expected one sink table, found {sinks}"
    print("ok: three triggers installed on the tracked table, sink created",
          flush=True)

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
    assert s1["captured_delta_syncs"] > 0, "no commit was served by an exact delta"
    assert s1["scan_syncs"] == s0["scan_syncs"], (
        f"a commit fell back to scan-and-diff: "
        f"{s1['scan_syncs']} scans (was {s0['scan_syncs']})"
    )
    print(
        f"ok: trigger_syncs={s1['trigger_syncs']} rows={s1['trigger_rows']} "
        f"exact_syncs={s1['captured_delta_syncs']} scans={s1['scan_syncs']}",
        flush=True,
    )

    # The bodies write one sink row per statement firing and nothing ever reads
    # them, so the drain has to keep the sink bounded on a long-lived process.
    for i in range(100):
        conn.execute(f"INSERT INTO items VALUES ({100 + i}, 'bulk', 1.0)")
    rows = conn.execute("SELECT count(*) FROM dbsp_trigger_sink").fetchone()[0]
    interval = int(os.environ["DBSP_TRIGGER_SINK_DRAIN"])
    assert rows <= 2 * interval, (
        f"sink grew unbounded: {rows} rows after 100 statements "
        f"(drain every {interval})"
    )
    got = conn.execute("SELECT * FROM dbsp_query('total')").fetchall()[0][0]
    want = conn.execute("SELECT SUM(price) FROM items").fetchone()[0]
    assert got == want, f"after the bulk inserts: view {got} != SQL {want}"
    print(f"ok: sink bounded at {rows} rows after 100 statements "
          f"(drain every {interval}), view still correct", flush=True)
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
        triggers = sorted(
            r[0]
            for r in conn.execute(
                "SELECT trigger_name FROM duckdb_triggers() WHERE table_name = 't'"
            ).fetchall()
        )
        assert len(triggers) == 3 and all(
            t.startswith("dbsp_trg_t_") for t in triggers
        ), f"triggers did not survive reopen: {triggers}"
        conn.execute("INSERT INTO t VALUES (2, 7.0)")
        got = conn.execute("SELECT * FROM dbsp_query('tot')").fetchall()[0][0]
        want = conn.execute("SELECT SUM(v) FROM t").fetchone()[0]
        assert got == want, f"after reopen: view {got} != SQL {want}"
        print("ok: reopen re-installs the triggers and the view stays correct",
              flush=True)
    finally:
        conn.close()

# ---- a triggered catalog must still be detachable ---------------------------
# Reading a catalog's version joins it to the statement's transaction, and
# DETACH refuses to run against a catalog the transaction has touched. Before
# the sweep learned to keep its hands off a DETACH, this threw
# "Cannot detach database m because the current transaction has outstanding
# work on it" on a connection that had only ever read from it.
with tempfile.TemporaryDirectory() as tmp:
    model = str(pathlib.Path(tmp) / "model.duckdb")
    setup = duckdb.connect(model)
    setup.execute("CREATE TABLE li (k INTEGER, v DOUBLE)")
    setup.execute("INSERT INTO li SELECT i % 3, i * 1.0 FROM range(6) t(i)")
    setup.close()

    conn = connect(":memory:")
    try:
        conn.execute(f"ATTACH '{model}' AS m (READ_WRITE)")
        conn.execute(
            "CREATE MATERIALIZED VIEW m_sum AS SELECT k, SUM(v) AS s FROM m.li GROUP BY k"
        )
        conn.execute("INSERT INTO m.li VALUES (0, 50.0)")
        got = dict(conn.execute("SELECT k, s FROM dbsp_query('m_sum')").fetchall())
        want = dict(conn.execute("SELECT k, SUM(v) FROM m.li GROUP BY k").fetchall())
        assert got == want, f"attached-catalog view {got} != SQL {want}"
        conn.execute("DETACH m")
        print("ok: an attached catalog with a tracked, triggered table detaches",
              flush=True)
    finally:
        conn.close()

signal.alarm(0)
print("PASS", flush=True)
