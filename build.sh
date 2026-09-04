#!/bin/bash
# Build script for DBSP DuckDB Extension
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
DUCKDB_VERSION="v2.0.0-alpha39998"
DUCKDB_COMMIT="a00803f7687ca3d7188d417216e288c5c4b22b58"

echo "=== DBSP DuckDB Extension Build ==="
echo ""

# Check if DuckDB source exists. Content check, not just the directory: a CI
# checkout of this repo leaves duckdb/ as an EMPTY submodule dir, and git
# commands run inside it silently resolve against THIS repo.
# Alpha engines are pinned by COMMIT (a tag would drift with the branch).
if [ ! -f "$SCRIPT_DIR/duckdb/CMakeLists.txt" ]; then
    echo "Fetching DuckDB ${DUCKDB_VERSION} (${DUCKDB_COMMIT})..."
    git init -q "$SCRIPT_DIR/duckdb"
    git -C "$SCRIPT_DIR/duckdb" remote add origin https://github.com/duckdb/duckdb.git
    git -C "$SCRIPT_DIR/duckdb" fetch --depth 1 origin "$DUCKDB_COMMIT"
    git -C "$SCRIPT_DIR/duckdb" checkout --detach FETCH_HEAD
fi

# Guard: duckdb/ must be its own git checkout (the version stamping and any
# git query below would otherwise run against the wrong repo).
DUCKDB_TOPLEVEL=$(git -C "$SCRIPT_DIR/duckdb" rev-parse --show-toplevel 2>/dev/null || true)
if [ "$DUCKDB_TOPLEVEL" != "$SCRIPT_DIR/duckdb" ]; then
    echo "ERROR: $SCRIPT_DIR/duckdb is not a standalone DuckDB checkout (toplevel: ${DUCKDB_TOPLEVEL:-none})." >&2
    exit 1
fi

# The engine tree is STOCK. There is no patch step: change capture comes from
# generated statement triggers, which are plain SQL objects the stock engine
# already supports. A dirty duckdb/ tree is therefore a mistake, not a build
# input — say so rather than compiling something nobody can reproduce.
if [ -n "$(git -C "$SCRIPT_DIR/duckdb" status --porcelain)" ]; then
    echo "ERROR: $SCRIPT_DIR/duckdb has local modifications; this fork builds against a STOCK engine." >&2
    echo "       Run: git -C \"$SCRIPT_DIR/duckdb\" checkout -- ." >&2
    exit 1
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Optional accelerators: use ccache/ninja when available (CI installs them;
# harmless to omit locally). Generator choice only affects a fresh build dir —
# an existing CMakeCache.txt keeps whatever generator configured it.
CMAKE_EXTRA_ARGS=()
if command -v ccache >/dev/null 2>&1; then
    CMAKE_EXTRA_ARGS+=("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache" "-DCMAKE_C_COMPILER_LAUNCHER=ccache")
fi
if command -v ninja >/dev/null 2>&1 && [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    CMAKE_EXTRA_ARGS+=("-GNinja")
fi

# Configure
echo "Configuring..."
DUCKDB_VERSION="$DUCKDB_VERSION" cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DDUCKDB_SOURCE_DIR="$SCRIPT_DIR/duckdb" \
    -DDUCKDB_EXPLICIT_VERSION="$DUCKDB_VERSION" \
    "${CMAKE_EXTRA_ARGS[@]}"

# Build (parallelism capped at 8 — higher has frozen this machine before)
if [[ "$OSTYPE" == "darwin"* ]]; then
    NCPU=$(sysctl -n hw.ncpu)
else
    NCPU=$(nproc)
fi
JOBS=$(( NCPU < 8 ? NCPU : 8 ))
echo "Building (-j${JOBS})..."
cmake --build . -j"${JOBS}"

# The DuckDB shell is NOT part of `all`: the root CMakeLists sets BUILD_SHELL ON
# but pulls the DuckDB tree in with EXCLUDE_FROM_ALL, so the `shell` target has to
# be named explicitly. verify_extension.sh looks for build/duckdb/duckdb first.
echo "Building DuckDB shell (-j${JOBS})..."
cmake --build . --target shell -j"${JOBS}"

echo ""
echo "=== Build Complete ==="
echo "Extension: $BUILD_DIR/dbsp.duckdb_extension"
echo ""
echo "Usage:"
echo "  duckdb -cmd \"LOAD '$BUILD_DIR/dbsp.duckdb_extension'\""
