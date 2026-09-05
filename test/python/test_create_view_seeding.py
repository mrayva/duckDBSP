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
                con.execute(prelude).fetchall()
            except Exception:
                pass  # the failure IS the setup
        con.execute("CREATE TABLE fresh (id INTEGER, v DOUBLE)")
        con.execute("INSERT INTO fresh VALUES (1, 3.0)")
        if prelude and not prelude_before_table:
            try:
                con.execute(prelude).fetchall()
            except Exception:
                pass
        # .fetchall() is LOAD-BEARING here, not style. A table function that
        # does its work in the execute callback does NOTHING until its result
        # is consumed: an unfetched dbsp_track leaves the table UNTRACKED —
        # measured, dbsp_tables() returns [] without the fetch and
        # [('…main.t', 2)] with it. Without these fetches every case below
        # exercised create_view's auto-track route and never the public-track
        # route this file is named for.
        con.execute("SELECT * FROM dbsp_track('fresh')").fetchall()
        tracked = con.execute("SELECT * FROM dbsp_tables()").fetchall()
        assert tracked, f"{label}: dbsp_track tracked nothing (unfetched?)"
        con.execute(
            "SELECT * FROM dbsp_create_view('tf', 'SELECT SUM(v) AS s FROM fresh')"
        ).fetchall()

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


# ---------------------------------------------------------------------------
# Part 2: the DEFERRED seeding path — a baseline that could not be scanned
# because the caller held a transaction open.
#
# seed_baseline leaves it empty and asks the connection to reconcile later.
# That debt has to survive BOTH commit branches: the trigger-fed fast path
# applies its buffered deltas and returns without consulting the transaction's
# sync scope, so recording the table there alone was silently discarded —
# measured view 3.0 against SQL 13.0, then 7.0 against 17.0, never healing.
# And a commit with auto-sync OFF reconciles nothing, so it must not throw the
# debt away either — measured view 4.0 against SQL 17.0 once auto-sync came
# back on.
#
# C++ sibling: `cdc: a deferred baseline is reconciled on EVERY commit path`.
# ---------------------------------------------------------------------------


def deferred_case(label, path, create_form, write_first):
    """Track `t` inside a transaction (seeding deferred, nothing reconciles it),
    let the sweep install the triggers, then create a view in a transaction the
    triggers DO feed."""
    con = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    try:
        con.execute(f"LOAD '{EXT}'")
        con.execute("CREATE TABLE t (id INTEGER, v DOUBLE)")
        con.execute("INSERT INTO t VALUES (1, 10.0)")
        con.execute("SELECT * FROM dbsp_auto_sync(false)").fetchall()
        con.execute("BEGIN TRANSACTION")
        con.execute("SELECT * FROM dbsp_track('t')").fetchall()
        con.execute("COMMIT")
        con.execute("SELECT 1").fetchall()  # statement boundary: triggers go in
        con.execute("SELECT * FROM dbsp_auto_sync(true)").fetchall()
        tracked = con.execute("SELECT * FROM dbsp_tables()").fetchall()
        assert tracked, f"{label}: nothing tracked"

        create = (
            "CREATE MATERIALIZED VIEW tv AS SELECT SUM(v) AS s FROM t"
            if create_form == "ddl"
            else "SELECT * FROM dbsp_create_view('tv','SELECT SUM(v) AS s FROM t')"
        )
        con.execute("BEGIN TRANSACTION")
        if write_first:
            con.execute("INSERT INTO t VALUES (2, 3.0)")
            con.execute(create).fetchall()
        else:
            con.execute(create).fetchall()
            con.execute("INSERT INTO t VALUES (2, 3.0)")
        con.execute("COMMIT")

        def both():
            v = con.execute("SELECT * FROM dbsp_query('tv')").fetchall()
            s = con.execute("SELECT SUM(v) FROM t").fetchall()
            return v, s

        v, s = both()
        assert v == s, f"{label}: view {v} != SQL {s} (reconcile discarded)"
        con.execute("INSERT INTO t VALUES (3, 4.0)")  # no constant offset
        v, s = both()
        assert v == s, f"{label} after edit: view {v} != SQL {s}"
        print(f"ok: {label} — view matches SQL ({v})", flush=True)
    finally:
        con.close()


def auto_sync_off_case(label, path, repair):
    """Defer the seeding with auto-sync OFF, so the commit reconciles nothing,
    then repair either by an explicit dbsp_sync() or by turning auto-sync back
    on and committing once."""
    con = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    try:
        con.execute(f"LOAD '{EXT}'")
        con.execute("CREATE TABLE t (id INTEGER, v DOUBLE)")
        con.execute("INSERT INTO t VALUES (1, 10.0)")
        con.execute("SELECT * FROM dbsp_auto_sync(false)").fetchall()
        con.execute("BEGIN TRANSACTION")
        con.execute("INSERT INTO t VALUES (2, 3.0)")
        con.execute(
            "SELECT * FROM dbsp_create_view('tv','SELECT SUM(v) AS s FROM t')"
        ).fetchall()
        con.execute("COMMIT")
        if repair == "sync":
            con.execute("SELECT * FROM dbsp_sync()").fetchall()
        else:
            con.execute("SELECT * FROM dbsp_auto_sync(true)").fetchall()
            con.execute("INSERT INTO t VALUES (3, 4.0)")
        v = con.execute("SELECT * FROM dbsp_query('tv')").fetchall()
        s = con.execute("SELECT SUM(v) FROM t").fetchall()
        assert v == s, f"{label}: view {v} != SQL {s} (deferred state dropped)"
        print(f"ok: {label} — view matches SQL ({v})", flush=True)
    finally:
        con.close()


def watermark_never_lowers_case(label, path):
    """A second deferral must not LOWER the readiness watermark.

    Two connections can defer the seeding of the SAME table, and the watermark
    says which transaction the debt is waiting on. Recording the newest
    deferral overwrote it, so an OLDER transaction's (lower) watermark cleared
    while the NEWER transaction was still open — and the reconcile scan then
    established the baseline from committed storage, missing exactly the rows
    that transaction had not committed yet. Measured before the fix:
    view [(10.0,)] against SQL [(13.0,)], and it never healed, because the
    write happened before the triggers existed and so fed nothing.

    B begins FIRST (older), A begins second and defers with the higher
    watermark, B defers with the lower one, then B commits — leaving A alone
    and open. The debt must still stand.
    """
    a = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    b = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    try:
        a.execute(f"LOAD '{EXT}'")
        b.execute(f"LOAD '{EXT}'")
        a.execute("CREATE TABLE t (id INTEGER, v DOUBLE)")
        a.execute("INSERT INTO t VALUES (1, 10.0)")

        b.execute("BEGIN TRANSACTION")           # older
        # Touch the tracked table's own catalog: DuckDB starts a per-database
        # transaction lazily, so BEGIN alone does not yet fix B's start
        # timestamp for that catalog.
        b.execute("SELECT count(*) FROM t").fetchall()
        a.execute("BEGIN TRANSACTION")           # newer
        a.execute("INSERT INTO t VALUES (2, 3.0)")   # uncommitted, untriggered
        a.execute(
            "SELECT * FROM dbsp_create_view('tv','SELECT SUM(v) AS s FROM t')"
        ).fetchall()                              # t tracked + DEFERRED (A's wm)
        b.execute(
            "SELECT * FROM dbsp_create_view('tv2','SELECT SUM(v) AS s FROM t')"
        ).fetchall()                              # defers again (B's LOWER wm)
        b.execute("COMMIT")                       # B's sweep must NOT establish

        a.execute("COMMIT")
        v = a.execute("SELECT * FROM dbsp_query('tv')").fetchall()
        sq = a.execute("SELECT SUM(v) FROM t").fetchall()
        assert v == sq, (
            f"{label}: view {v} != SQL {sq} — a lowered watermark let the "
            f"baseline be established while the deferring transaction was open"
        )
        a.execute("INSERT INTO t VALUES (3, 4.0)")   # and no constant offset
        v = a.execute("SELECT * FROM dbsp_query('tv')").fetchall()
        sq = a.execute("SELECT SUM(v) FROM t").fetchall()
        assert v == sq, f"{label} after edit: view {v} != SQL {sq}"
        print(f"ok: {label} — the watermark never lowers ({v})", flush=True)
    finally:
        b.close()
        a.close()


def in_window_sync_case(label, path):
    """`dbsp_sync()` INSIDE the deferring transaction cannot pay the debt.

    The scan it runs opens its own connection, so it cannot see the open
    transaction's uncommitted rows — establishing a baseline from it would
    serve a view short by exactly those rows. It used to do precisely that:
    `finish_rebuild()` marked the baseline seeded unconditionally, so an
    in-window `dbsp_sync()` retired the debt and the next read was served the
    short answer. Now the scan REFRESHES without establishing, the read still
    refuses, and — the half that keeps this from being a silent no-op —
    `dbsp_sync()` says what it could not do instead of reporting success.
    """
    a = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    b = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    try:
        a.execute(f"LOAD '{EXT}'")
        b.execute(f"LOAD '{EXT}'")
        a.execute("CREATE TABLE t (id INTEGER, v DOUBLE)")
        a.execute("INSERT INTO t VALUES (1, 10.0)")
        a.execute("BEGIN TRANSACTION")
        a.execute("INSERT INTO t VALUES (2, 3.0)")
        a.execute(
            "SELECT * FROM dbsp_create_view('tv','SELECT SUM(v) AS s FROM t')"
        ).fetchall()

        status = a.execute("SELECT * FROM dbsp_sync()").fetchall()[0][0]
        assert "still owed" in status, (
            f"{label}: dbsp_sync() reported {status!r} for a call that paid "
            f"nothing"
        )

        # The other connection must still be refused: the baseline is short by
        # A's uncommitted rows and no scan can find them.
        try:
            rows = b.execute("SELECT * FROM dbsp_query('tv')").fetchall()
            raise AssertionError(
                f"{label}: B was served {rows} from a baseline the in-window "
                f"sync could not establish"
            )
        except duckdb.InvalidInputException as e:
            assert "baseline" in str(e), f"{label}: unexpected error {e}"

        # The debt is paid where it can be: at the transaction's own COMMIT.
        a.execute("COMMIT")
        for who, con in (("A", a), ("B", b)):
            v = con.execute("SELECT * FROM dbsp_query('tv')").fetchall()
            sq = con.execute("SELECT SUM(v) FROM t").fetchall()
            assert v == sq, f"{label}/{who} after COMMIT: view {v} != SQL {sq}"
        print(f"ok: {label} — in-window dbsp_sync() is honest, and the commit "
              f"pays", flush=True)
    finally:
        b.close()
        a.close()


with tempfile.TemporaryDirectory() as tmp:
    n = 0
    for mode in ("file", "memory"):
        for form in ("ddl", "function"):
            for write_first in (True, False):
                n += 1
                path = (":memory:" if mode == "memory"
                        else os.path.join(tmp, f"d_{n}.duckdb"))
                order = "write_first" if write_first else "create_first"
                deferred_case(f"{mode}/{form}_{order}", path, form, write_first)
        for repair in ("sync", "auto_sync_back_on"):
            n += 1
            path = (":memory:" if mode == "memory"
                    else os.path.join(tmp, f"d_{n}.duckdb"))
            auto_sync_off_case(f"{mode}/auto_sync_off_{repair}", path, repair)
    # Two connections against one file: an in-window dbsp_sync() must not
    # establish, and must not claim it did.
    in_window_sync_case("in_window_sync",
                        os.path.join(tmp, "in_window.duckdb"))
    watermark_never_lowers_case("watermark_never_lowers",
                                os.path.join(tmp, "wm_lower.duckdb"))


# ---------------------------------------------------------------------------
# Part 3: the deferral window is visible to OTHER connections.
#
# The deferred-seeding debt is per-connection; the baseline it refers to is
# per-INSTANCE. While connection A holds the transaction that deferred the
# seed, connection B's commits took the trigger-fed fast path and applied their
# exact deltas onto A's still-EMPTY baseline, so B read the deltas alone —
# measured `view 1.0 / sql 11.0`, `2.0 / 12.0`, `3.0 / 13.0` over three of B's
# commits and `100.0 / 110.0` for one. Transient (A's commit healed it) but a
# wrong answer with no error, which the invariant forbids.
#
# File-backed only: DuckDB's `:memory:` is per-connection, so two connections
# over one instance need a file. C++ sibling: `cdc: an unseeded baseline is
# never served to another connection`.
# ---------------------------------------------------------------------------


def _deferred_window(a):
    """Leave `t` tracked, triggers installed, baseline UNSEEDED, on conn a."""
    a.execute("CREATE TABLE t (id INTEGER, v DOUBLE)")
    a.execute("INSERT INTO t VALUES (1, 10.0)")
    a.execute("SELECT * FROM dbsp_auto_sync(false)").fetchall()
    a.execute("BEGIN TRANSACTION")
    a.execute("SELECT * FROM dbsp_track('t')").fetchall()
    a.execute("COMMIT")
    a.execute("SELECT 1").fetchall()
    a.execute("SELECT * FROM dbsp_auto_sync(true)").fetchall()


def cross_connection_case(label, path, shape):
    a = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    b = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    try:
        a.execute(f"LOAD '{EXT}'")
        b.execute(f"LOAD '{EXT}'")
        _deferred_window(a)
        b.execute("SELECT 1").fetchall()

        def agree(who, tag):
            v = who.execute("SELECT * FROM dbsp_query('tv')").fetchall()
            s = who.execute("SELECT SUM(v) FROM t").fetchall()
            assert v == s, f"{label}/{tag}: view {v} != SQL {s}"
            return v

        a.execute("BEGIN TRANSACTION")
        if shape == "a_writes_too":
            a.execute("INSERT INTO t VALUES (2, 3.0)")
        a.execute(
            "SELECT * FROM dbsp_create_view('tv','SELECT SUM(v) AS s FROM t')"
        ).fetchall()

        if shape == "repeated":
            for i in range(3):
                b.execute(f"INSERT INTO t VALUES ({20 + i}, 1.0)")
                agree(b, f"B write {i}")     # 11.0, 12.0, 13.0
            a.execute("COMMIT")
        elif shape == "b_in_txn":
            b.execute("BEGIN TRANSACTION")
            b.execute("INSERT INTO t VALUES (5, 100.0)")
            b.execute("COMMIT")
            agree(b, "B explicit commit")    # 110.0
            a.execute("COMMIT")
        elif shape == "rollback":
            b.execute("INSERT INTO t VALUES (5, 100.0)")
            agree(b, "B write while A open")  # 110.0
            a.execute("ROLLBACK")
            b.execute("SELECT 1").fetchall()
            agree(b, "after A rollback")
            b.execute("INSERT INTO t VALUES (6, 1.0)")
            agree(b, "next B write")
        else:  # a_writes_too / plain
            b.execute("INSERT INTO t VALUES (5, 100.0)")
            agree(b, "B write while A open")  # 110.0
            a.execute("COMMIT")
        v = agree(a, "final")
        a.execute("INSERT INTO t VALUES (7, 4.0)")  # no constant offset
        agree(a, "after a later edit")
        print(f"ok: {label} — view matches SQL at every step ({v})", flush=True)
    finally:
        a.close()
        b.close()


with tempfile.TemporaryDirectory() as tmp:
    for i, shape in enumerate(("a_writes_too", "plain", "repeated",
                               "b_in_txn", "rollback")):
        cross_connection_case(f"cross/{shape}",
                              os.path.join(tmp, f"x_{i}.duckdb"), shape)

drain = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
drain.execute(f"LOAD '{EXT}'")
drain.execute("SELECT * FROM dbsp_wait_teardown()").fetchall()
drain.close()

signal.alarm(0)
print("PASS", flush=True)
