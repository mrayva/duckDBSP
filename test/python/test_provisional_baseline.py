"""A baseline seeded while another connection had a transaction open.

The seeding scan reads COMMITTED storage. If another connection already holds
a transaction that wrote this table while it was UNTRACKED, that write is
invisible to the scan (uncommitted) and fired no trigger (there were no
triggers when the statement ran). Its commit therefore reports nothing, and the
view is short by those rows forever:

    after A commit:     view 10.0   sql 13.0
    after a later edit: view 14.0   sql 17.0

The fix is a per-INSTANCE transaction watermark. At seed time the table is
marked PROVISIONAL if DuckDB's transaction manager says a transaction OLDER
than the seeding one is active (`LowestActiveStart() < my start_time`), with a
watermark taken as the start timestamp of a transaction begun on the spot —
newer than every transaction alive then. While provisional, no connection's
commit applies an exact delta to the table; it is handed to the scan-reconcile
instead. The first commit taken after `LowestActiveStart()` has risen to the
watermark scans it once and retires the flag — including a commit that touched
nothing, which is the case this reproduction needs, since at A's OWN commit A's
transaction is still active.

Covered here: the exact reproduction (file and :memory:), the ROLLBACK variant
(A's write never lands, and the view must not gain it), that a seeding with no
other transaction open pays NO extra scan and is never provisional, and a
NumPad-shaped control — a second connection that only ever reads and never
opens a transaction of its own — which must also pay nothing.

Run: python test_provisional_baseline.py <path-to-ext>
"""

import os
import signal
import sys
import tempfile

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 180
fails = []


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)


def check(cond, msg):
    if not cond:
        fails.append(msg)
    print(f"  {'ok  ' if cond else 'FAIL'} {msg}", flush=True)


def stats(con):
    # dbsp_stats() has THREE columns: metric, value, detail. Only
    # last_reconcile_error uses detail; every other row is NULL there.
    return {name: value for name, value, _ in
            con.execute("SELECT * FROM dbsp_stats()").fetchall()}


def agree(label, con):
    view = con.execute("SELECT * FROM dbsp_query('tv')").fetchall()
    sql = con.execute("SELECT sum(v) FROM t").fetchall()
    check(view == sql, f"{label}: view {view} == sql {sql}")


def repro(label, a, b, end):
    """A holds an open transaction with a PRE-TRACKING write; B tracks."""
    print(f"== {label} (A ends with {end})", flush=True)
    a.execute("CREATE TABLE t (id INTEGER, v DOUBLE)")
    a.execute("INSERT INTO t VALUES (1, 10.0)")
    a.execute("BEGIN TRANSACTION")
    a.execute("INSERT INTO t VALUES (2, 3.0)")  # UNTRACKED and uncommitted
    b.execute("SELECT * FROM dbsp_track('t')").fetchall()
    b.execute(
        "SELECT * FROM dbsp_create_view('tv', 'SELECT sum(v) AS s FROM t')"
    ).fetchall()
    check(stats(b)["provisional_tables"] == 1,
          f"{label}: the seed was marked provisional")

    a.execute(end)
    # A statement on B whose commit finds the watermark clear: this is the one
    # that pays the repair scan. It writes nothing.
    b.execute("SELECT 1").fetchall()
    agree(f"{label}/after A {end}", b)
    check(stats(b)["provisional_tables"] == 0,
          f"{label}: the flag was retired after the repair scan")

    # And it stays exact through later edits from both connections.
    b.execute("INSERT INTO t VALUES (3, 4.0)")
    agree(f"{label}/after B edit", b)
    a.execute("INSERT INTO t VALUES (4, 5.0)")
    agree(f"{label}/after A edit", a)


def no_other_txn(label, a, b):
    """Seeding with nothing else open must not go provisional or cost a scan.

    b is a NumPad-shaped control: a second connection that reads and never
    opens a transaction of its own. It must not make the seed provisional
    either.
    """
    print(f"== {label}", flush=True)
    a.execute("CREATE TABLE t (id INTEGER, v DOUBLE)")
    a.execute("INSERT INTO t VALUES (1, 10.0)")
    b.execute("SELECT count(*) FROM t").fetchall()  # reader, autocommit only
    before = stats(a)
    a.execute("SELECT * FROM dbsp_track('t')").fetchall()
    a.execute(
        "SELECT * FROM dbsp_create_view('tv', 'SELECT sum(v) AS s FROM t')"
    ).fetchall()
    after_seed = stats(a)
    check(after_seed["provisional_tables"] == 0,
          f"{label}: seeding with no other transaction open is not provisional")
    agree(f"{label}/after seed", a)

    # Six edits, each of which must be served by an exact delta and cost no
    # scan. A provisional table would scan on every one of them.
    base = stats(a)
    for i in range(3):
        a.execute(f"INSERT INTO t VALUES ({10 + i}, 1.0)")
        agree(f"{label}/A edit {i}", a)
        b.execute(f"INSERT INTO t VALUES ({20 + i}, 1.0)")
        agree(f"{label}/B edit {i}", b)
    end = stats(a)
    scans = end["scan_syncs"] - base["scan_syncs"]
    exact = end["exact_delta_syncs"] - base["exact_delta_syncs"]
    check(scans == 0,
          f"{label}: six edits cost {scans} scans (want 0)")
    check(exact == 6,
          f"{label}: six edits applied {exact} exact deltas (want 6)")
    check(end["provisional_tables"] == 0,
          f"{label}: still not provisional after six edits")
    # The seeding itself scans once, and that is all it should cost.
    seed_scans = after_seed["scan_syncs"] - before["scan_syncs"]
    check(seed_scans <= 1,
          f"{label}: seeding cost {seed_scans} scans (want at most 1)")


def connect(path):
    con = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")
    return con


def run(label, body, backend, tmp, n):
    if backend == "file":
        path = os.path.join(tmp, f"prov_{n}.duckdb")
        a, b = connect(path), connect(path)
    else:
        a = connect(":memory:")
        b = a.cursor()
        b.execute(f"LOAD '{EXT}'")
    try:
        body(f"{backend}/{label}", a, b)
    finally:
        b.close()
        a.close()


with tempfile.TemporaryDirectory() as tmp:
    n = 0
    for backend in ("file", "mem"):
        run("commit", lambda l, a, b: repro(l, a, b, "COMMIT"), backend, tmp, n)
        n += 1
        run("rollback", lambda l, a, b: repro(l, a, b, "ROLLBACK"), backend,
            tmp, n)
        n += 1
        run("solo", no_other_txn, backend, tmp, n)
        n += 1

drain = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
drain.execute(f"LOAD '{EXT}'")
drain.execute("SELECT * FROM dbsp_wait_teardown()").fetchall()
drain.close()

print("\nFAILURES:" if fails else "\nPASS: provisional baselines reconcile, "
                                  "solo seeding costs nothing")
for f in fails:
    print(" ", f)
sys.exit(1 if fails else 0)
