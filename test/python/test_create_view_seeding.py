"""create_view must seed its source baseline from committed storage.

The public `dbsp_track()` creates a tracked table with an EMPTY baseline on
purpose and tells the caller to run `dbsp_sync()`. `create_view`'s replay
streams that baseline, so a view created straight after a track was built over
NOTHING unless some unrelated commit happened to scan-sync the table first.

That accident is removed by any earlier FAILED DBSP call, and the result was a
permanently wrong answer with no error, no counter moving, and no self-healing:
    view [(None,)]  sql [(3.0,)]
    after insert   view 3.0  / sql 6.0
    after insert2  view 7.0  / sql 10.0     <- the same constant offset forever

Covered here on a FILE-BACKED database (where it was found) as well as
:memory:. The C++ sibling is `cdc: create_view seeds its source baseline, not
an empty one` in test/unit/test_trigger_source.cpp.

Run: python test_create_view_seeding.py <path-to-ext>
"""

import os
import signal
import sys
import tempfile

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 120


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)

BAD_TRACK = "SELECT * FROM dbsp_track('no_such_table')"
BAD_VIEW = "SELECT * FROM dbsp_create_view('bad', 'SELECT 1 FROM no_such_table')"


def run_case(label, path, prelude, prelude_before_table):
    con = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    try:
        con.execute(f"LOAD '{EXT}'")
        if prelude and prelude_before_table:
            try:
                con.execute(prelude)
            except Exception:
                pass  # the failure IS the setup
        con.execute("CREATE TABLE fresh (id INTEGER, v DOUBLE)")
        con.execute("INSERT INTO fresh VALUES (1, 3.0)")
        if prelude and not prelude_before_table:
            try:
                con.execute(prelude)
            except Exception:
                pass
        con.execute("SELECT * FROM dbsp_track('fresh')")
        con.execute(
            "SELECT * FROM dbsp_create_view('tf', 'SELECT SUM(v) AS s FROM fresh')"
        )

        def both():
            v = con.execute("SELECT * FROM dbsp_query('tf')").fetchall()
            s = con.execute("SELECT SUM(v) FROM fresh").fetchall()
            return v, s

        v, s = both()
        assert v == s, f"{label}: view {v} != SQL {s} (baseline never seeded)"
        # and no constant offset carried forward
        con.execute("INSERT INTO fresh VALUES (2, 3.0)")
        v, s = both()
        assert v == s, f"{label} after insert: view {v} != SQL {s}"
        con.execute("INSERT INTO fresh VALUES (3, 4.0)")
        v, s = both()
        assert v == s, f"{label} after insert2: view {v} != SQL {s}"
        print(f"ok: {label} — view matches SQL at every step ({v})", flush=True)
    finally:
        con.close()


with tempfile.TemporaryDirectory() as tmp:
    for mode in ("file", "memory"):
        n = 0
        for prelude, pname in ((None, "no_prior_failure"),
                               (BAD_TRACK, "failed_track"),
                               (BAD_VIEW, "failed_view")):
            for first in (True, False):
                if prelude is None and not first:
                    continue
                n += 1
                path = (":memory:" if mode == "memory"
                        else os.path.join(tmp, f"m_{mode}_{n}.duckdb"))
                where = "before" if first else "after"
                run_case(f"{mode}/{pname}_{where}_create", path, prelude, first)

signal.alarm(0)
print("PASS", flush=True)
