"""A read surface must refuse a view whose source baseline is unseeded.

Seeding is DEFERRED while a user transaction is open: the seeding scan runs on
an internal connection, which cannot see that transaction's own rows, so
`CDCManager::seed_baseline` leaves the baseline empty and records the debt.
The apply path already refuses to apply an exact delta onto such a baseline.
A PURE READ did not, and returned the empty answer:

    in-window A: dbsp_query('mv') -> [(None,)]   plain SQL -> 10.0
    in-window B: dbsp_query('mv') -> [(None,)]   plain SQL -> 10.0

on the connection holding `BEGIN; dbsp_create_view(...)` open AND on every
other connection of the instance, on :memory: and on a file, healing only when
that transaction ended. `dbsp_changes` was worse: it served
`[(None, -1), (10.0, 1)]` on the second connection while `dbsp_query` on the
same connection served NULL.

Both surfaces now throw, naming the deferring state. Once the window ends —
by COMMIT (whose hook sweeps the tables that owe a seeding scan) or by ROLLBACK
(after which the first read runs that same sweep itself, because the table it
would scan is finally ready) — the reads work and are EXACT.
The assertions below compare against plain SQL, so a constant offset cannot
hide, and they run again after a later edit on each connection.

Run: python test_unseeded_read.py <path-to-ext>
"""

import os
import signal
import sys
import tempfile

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 120
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


def refuses(con, sql, label):
    """The call must throw, and the message must name the unseeded state."""
    try:
        rows = con.execute(sql).fetchall()
    except Exception as e:
        text = str(e)
        check("UNSEEDED baseline" in text and "main.t" in text,
              f"{label}: refused, message names the state and the table "
              f"({text.splitlines()[0][:90]})")
        return
    fails.append(f"{label}: returned {rows} instead of refusing")
    print(f"  FAIL {label}: returned {rows} instead of refusing", flush=True)


def case(label, a, b, probe_reads):
    """probe_reads=True asserts the refusals; probe_reads=False leaves the
    window untouched so the transaction reaches its own COMMIT intact.

    Both shapes are needed. A refused read THROWS, and a thrown error aborts
    the enclosing DuckDB transaction — so the run that proves the refusal
    cannot also prove that the deferring transaction's own uncommitted write
    survives into the view. The second shape does that.
    """
    print(f"== {label} (probe_reads={probe_reads})", flush=True)
    a.execute("CREATE TABLE t (id INTEGER, v DOUBLE)")
    a.execute("INSERT INTO t VALUES (1, 10.0)")
    a.execute("BEGIN TRANSACTION")
    a.execute("INSERT INTO t VALUES (2, 3.0)")  # uncommitted, invisible to the seed
    # create_view AUTO-TRACKS t here, inside the open transaction, so the
    # seeding scan is deferred and the baseline is genuinely never scanned.
    a.execute(
        "SELECT * FROM dbsp_create_view('mv', 'SELECT sum(v) AS s FROM t')"
    ).fetchall()

    if probe_reads:
        refuses(a, "SELECT * FROM dbsp_query('mv')", f"{label}/same-conn query")
        refuses(a, "SELECT * FROM dbsp_changes('mv')", f"{label}/same-conn changes")
        refuses(b, "SELECT * FROM dbsp_query('mv')", f"{label}/other-conn query")
        refuses(b, "SELECT * FROM dbsp_changes('mv')", f"{label}/other-conn changes")
        # The refusals aborted A's transaction — DuckDB requires it be rolled
        # back, so this shape ends the window by ROLLBACK rather than COMMIT.
        # A rollback runs no commit hook, so the repair falls to the READ path,
        # which runs the same sweep before refusing: the table's readiness
        # watermark has cleared now that the transaction is gone, so the first
        # read scans it and answers. The reads below have to work either way.
        a.execute("ROLLBACK")
    else:
        a.execute("COMMIT")

    # The window is over and the debt is paid: both connections read, and read
    # the same thing plain SQL does.
    for who, con in (("A", a), ("B", b)):
        view = con.execute("SELECT * FROM dbsp_query('mv')").fetchall()
        sql = con.execute("SELECT sum(v) FROM t").fetchall()
        check(view == sql, f"{label}/after-window {who}: view {view} == sql {sql}")

    # And it stays exact through a later edit on each connection — a baseline
    # short by the deferred rows would show as a constant offset here.
    a.execute("INSERT INTO t VALUES (7, 4.0)")
    view = a.execute("SELECT * FROM dbsp_query('mv')").fetchall()
    sql = a.execute("SELECT sum(v) FROM t").fetchall()
    check(view == sql, f"{label}/after A edit: view {view} == sql {sql}")
    b.execute("INSERT INTO t VALUES (8, 5.0)")
    view = b.execute("SELECT * FROM dbsp_query('mv')").fetchall()
    sql = b.execute("SELECT sum(v) FROM t").fetchall()
    check(view == sql, f"{label}/after B edit: view {view} == sql {sql}")


def connect(path):
    con = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")
    return con


with tempfile.TemporaryDirectory() as tmp:
    for i, probe in enumerate((True, False)):
        path = os.path.join(tmp, f"unseeded_{i}.duckdb")
        a = connect(path)
        b = connect(path)
        try:
            case(f"file/probe{int(probe)}", a, b, probe)
        finally:
            b.close()
            a.close()

    for probe in (True, False):
        a = connect(":memory:")
        b = a.cursor()
        b.execute(f"LOAD '{EXT}'")
        try:
            case(f"mem/probe{int(probe)}", a, b, probe)
        finally:
            b.close()
            a.close()

drain = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
drain.execute(f"LOAD '{EXT}'")
drain.execute("SELECT * FROM dbsp_wait_teardown()").fetchall()
drain.close()

print("\nFAILURES:" if fails else "\nPASS: unseeded reads refuse, seeded reads are exact")
for f in fails:
    print(" ", f)
sys.exit(1 if fails else 0)
