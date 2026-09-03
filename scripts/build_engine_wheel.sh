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
# Usage: scripts/build_engine_wheel.sh [duckdb-python-checkout]
#   default checkout: ../duckdb-python (cloned at $DUCKDB_PYTHON_REF)
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
  echo "cloning duckdb-python $DUCKDB_PYTHON_REF -> $PYPKG"
  git clone --branch "$DUCKDB_PYTHON_REF" --depth 1 https://github.com/duckdb/duckdb-python.git "$PYPKG"
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
# Cap build parallelism: ninja defaults to all cores; unbounded clang on a
# 16GB machine swap-storms.
export OVERRIDE_GIT_DESCRIBE="${OVERRIDE_GIT_DESCRIBE:-v1.6.0-post1}"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
uv build --wheel

echo
ls -la dist/*.whl
echo "smoke: uv pip install dist/*.whl && python -c \"import duckdb; assert duckdb.__version__.endswith('.post1'), duckdb.__version__; print(duckdb.__version__)\""
