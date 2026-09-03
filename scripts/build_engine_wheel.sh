#!/usr/bin/env bash
# Build the patched (dbsp engine-hook) DuckDB Python wheel locally.
#
# The Python client lives in the separate duckdb/duckdb-python repo (the
# engine tree has no tools/pythonpkg since the 1.2/1.3 split). Its
# external/duckdb submodule must pin the same engine commit as ours
# (v2.0.0-alpha39998 = a00803f7) for
# patches/v2.0.0-alpha39998-dbsp-txn-callback.patch to apply 1:1.
#
# The registry lives in the DatabaseInstance ObjectCache, so a hook-built
# extension also loads on a stock engine (it then runs the capture stack);
# the patched wheel is what makes the hook fire.
#
# Usage: DUCKDB_PYTHON_REF=<sha> scripts/build_engine_wheel.sh [duckdb-python-checkout]
#   default checkout: ../duckdb-python (cloned at $DUCKDB_PYTHON_REF)
#
# The pinned ref (found and verified 2026-09-03):
#   DUCKDB_PYTHON_REF=d483a612d1e225c6b9293fd5733a2b76f5109049
# duckdb-python "Bump submodule" (2026-09-02) — the FIRST commit on main whose
# external/duckdb gitlink is a00803f7, from the day PyPI published the paired
# duckdb 1.6.0.dev379. Deliberately a commit, not `main`: main's gitlink moves,
# and the two commits right after it ("New versioning scheme", 2026-09-03)
# rewrite duckdb_packaging/ and delete setuptools_scm_version.py, which is the
# file that reads OVERRIDE_GIT_DESCRIBE below.
set -euo pipefail

DBSP_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PYPKG="${1:-$DBSP_ROOT/../duckdb-python}"
PATCH="$DBSP_ROOT/patches/v2.0.0-alpha39998-dbsp-txn-callback.patch"
# NO DEFAULT ON PURPOSE. The alpha has no matching client tag yet, so this is
# a branch/commit, and `main` is NOT it: main's external/duckdb gitlink moves,
# so defaulting to it would clone an arbitrary engine, apply the patch to it (or
# fail), and label the result as the paired artifact. Fail loudly instead.
: "${DUCKDB_PYTHON_REF:?set to the duckdb-python ref whose external/duckdb gitlink is a00803f7687ca3d7188d417216e288c5c4b22b58 (Task 10 owns this)}"

if [ ! -d "$PYPKG" ]; then
  # fetch-by-ref, not `clone --branch`: the pinned ref is a raw commit sha and
  # --branch only accepts branch/tag names.
  echo "cloning duckdb-python $DUCKDB_PYTHON_REF -> $PYPKG"
  git init -q "$PYPKG"
  git -C "$PYPKG" remote add origin https://github.com/duckdb/duckdb-python.git
  git -C "$PYPKG" fetch --depth 1 origin "$DUCKDB_PYTHON_REF"
  git -C "$PYPKG" checkout -q --detach FETCH_HEAD
fi
cd "$PYPKG"
git submodule update --init --depth 1 external/duckdb

# apply the engine patch (idempotent: skip if already applied)
if git -C external/duckdb apply --check "$PATCH" 2>/dev/null; then
  git -C external/duckdb apply "$PATCH"
  echo "engine patch applied"
else
  git -C external/duckdb apply --reverse --check "$PATCH" 2>/dev/null \
    && echo "engine patch already applied" \
    || { echo "ERROR: patch neither applies nor is applied — engine tree diverged"; exit 1; }
fi

# Fork version marker: the client's custom backend (duckdb_packaging)
# IGNORES SETUPTOOLS_SCM_PRETEND_VERSION* and supports only
# OVERRIDE_GIT_DESCRIBE in vX.Y.Z[-postN] form — local segments (+dbsp)
# are impossible, so the fork ships as a .post1 of the client's own version.
# The client series is 1.6.0 (PyPI ships the alpha engine as duckdb
# 1.6.0.dev379), NOT 2.0.0 — 2.0.0 is the ENGINE version. So the fork wheel is
# v1.6.0-post1, which installs as 1.6.0.post1 and sorts above the PyPI dev
# build it replaces.
#
# OVERRIDE_DUCKDB_GIT_DESCRIBE is NOT optional. OVERRIDE_GIT_DESCRIBE alone
# also renames the ENGINE (forced_duckdb_version_from_env carries the package
# version over, minus the post suffix), so the library reports itself as
# 'v1.6.0' and REFUSES our extension:
#   Failed to load dbsp.duckdb_extension, The file was built specifically for
#   DuckDB version 'v2.0.0-alpha39998' ... (this version of DuckDB is 'v1.6.0')
# The extension loader matches that string exactly, so the engine must keep its
# real describe while the package takes the .post1 marker.
# Cap build parallelism: ninja defaults to all cores; unbounded clang on a
# 16GB machine swap-storms.
export OVERRIDE_GIT_DESCRIBE="${OVERRIDE_GIT_DESCRIBE:-v1.6.0-post1}"
export OVERRIDE_DUCKDB_GIT_DESCRIBE="${OVERRIDE_DUCKDB_GIT_DESCRIBE:-v2.0.0-alpha39998}"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
uv build --wheel

echo
WHEEL="$(ls -t dist/duckdb-*.whl | head -1)"
ls -la "$WHEEL"

# Smoke the pair, not just the wheel. A wheel that imports but cannot load the
# extension is the failure mode this build has already hit once (see the
# OVERRIDE_DUCKDB_GIT_DESCRIBE note above), and it is invisible until NumPad
# opens a model.
# --no-project: we are cwd'd inside duckdb-python, and without it uv resolves
# THAT project's dev dependency groups (torch, pyspark, ...) — GBs of download
# for a two-line check.
uv run --no-project --isolated --with "$PWD/$WHEEL" python - "$DBSP_ROOT/build/dbsp.duckdb_extension" <<'PY'
import sys, duckdb
assert duckdb.__version__.endswith(".post1"), duckdb.__version__
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute(f"LOAD '{sys.argv[1]}'")
ver, src, _ = con.sql("pragma version").fetchall()[0]
print(f"OK  package={duckdb.__version__}  engine={ver}  source={src}  extension loaded")
con.close()
PY
