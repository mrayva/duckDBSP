"""A failed reconcile scan must be visible to the caller, not just on stderr.

`sync_table_scan_and_consume` reports failure by RETURNING `nullopt`, not by
throwing, so nothing propagates out of the commit hook: a view left stale by a
failed reconcile was announced only on stderr and in `last_error_`, neither of
which a host embedding the extension sees. `dbsp_stats()` now carries
`reconcile_failures` (a count) and `last_reconcile_error` (the text, in the
`detail` column).

The scenario is round 5's: a view is created inside an open transaction, which
DEFERS the seeding scan and leaves a debt on the connection; the same
transaction then DROPS the source table; the COMMIT widens itself to a full
scan-and-diff to pay the debt and the scan cannot run, because the table is
gone:

    DBSP: reconcile scan did not run for 'db.main.t'; its baseline is
    unchanged. Last error: Exception in sync_table_scan_and_consume: Failed to
    scan table 'db.main.t': Catalog Error: Table with name t does not exist!

Before this change `reconcile_failures` did not exist and `dbsp_stats()` had
two columns; the failure was invisible from SQL.

Run: python test_reconcile_telemetry.py <path-to-ext>
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


def stats(con):
    rows = con.execute("SELECT * FROM dbsp_stats()").fetchall()
    return {name: (value, detail) for name, value, detail in rows}


def case(label, con):
    print(f"== {label}", flush=True)
    s = stats(con)
    check("reconcile_failures" in s and "last_reconcile_error" in s,
          f"{label}: dbsp_stats() carries both rows")
    check(s["reconcile_failures"][0] == 0,
          f"{label}: no failures on a fresh instance "
          f"(got {s['reconcile_failures'][0]})")
    check(s["last_reconcile_error"][1] is None,
          f"{label}: last_reconcile_error is NULL with nothing to report "
          f"(got {s['last_reconcile_error'][1]!r})")
    # The VALUE column of a text-only row is NULL, not a copy of the
    # reconcile_failures count. It repeated the count once, which invited it to
    # be read as a number of its own.
    check(s["last_reconcile_error"][0] is None,
          f"{label}: last_reconcile_error.value is NULL "
          f"(got {s['last_reconcile_error'][0]!r})")

    con.execute("CREATE TABLE t (id INTEGER, v DOUBLE)")
    con.execute("INSERT INTO t VALUES (1, 10.0)")
    con.execute("BEGIN TRANSACTION")
    # Inside the transaction the seeding scan is deferred and the debt is
    # recorded on this connection.
    con.execute(
        "SELECT * FROM dbsp_create_view('mv', 'SELECT sum(v) AS s FROM t')"
    ).fetchall()
    # ... and then the source goes away, so the reconcile that pays the debt
    # cannot run.
    con.execute("DROP TABLE t")
    con.execute("COMMIT")

    s = stats(con)
    check(s["reconcile_failures"][0] >= 1,
          f"{label}: the failed reconcile was counted "
          f"(reconcile_failures={s['reconcile_failures'][0]})")
    text = s["last_reconcile_error"][1] or ""
    check("reconcile scan did not run" in text and "main.t" in text,
          f"{label}: last_reconcile_error names the table and the failure "
          f"({text[:110]!r})")
    check(s["last_reconcile_error"][0] is None,
          f"{label}: last_reconcile_error.value stays NULL once it has text "
          f"(got {s['last_reconcile_error'][0]!r})")


with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "telemetry.duckdb")
    con = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT}'")
    try:
        case("file", con)
    finally:
        con.close()

con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
con.execute(f"LOAD '{EXT}'")
try:
    case(":memory:", con)
finally:
    con.execute("SELECT * FROM dbsp_wait_teardown()").fetchall()
    con.close()

print("\nFAILURES:" if fails else "\nPASS: a failed reconcile is visible in "
                                  "dbsp_stats()")
for f in fails:
    print(" ", f)
sys.exit(1 if fails else 0)
