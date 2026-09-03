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
# PLACEHOLDER — Task 10 sets this to the duckdb-python ref whose
# external/duckdb submodule pins engine commit a00803f7 (v2.0.0-alpha39998).
# The alpha has no matching client tag yet, so it is a branch/commit, not a tag.
DUCKDB_PYTHON_REF="${DUCKDB_PYTHON_REF:-main}"

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
# PLACEHOLDER — Task 10 pins this alongside DUCKDB_PYTHON_REF.
# Cap build parallelism: ninja defaults to all cores; unbounded clang on a
# 16GB machine swap-storms.
export OVERRIDE_GIT_DESCRIBE="${OVERRIDE_GIT_DESCRIBE:-v2.0.0-post1}"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
uv build --wheel

echo
ls -la dist/*.whl
echo "smoke: uv pip install dist/*.whl && python -c \"import duckdb; print(duckdb.__version__)\""
