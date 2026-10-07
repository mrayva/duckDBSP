cmake_minimum_required(VERSION 3.14)

project(dbsp_extension)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TARGET_NAME dbsp)
set(EXT_VERSION_DBSP "0.1.0")
option(DBSP_USE_PARENT_DUCKDB
       "Use DuckDB targets from the containing extension build" OFF)

# DuckDB source directory (set via command line or default)
set(DUCKDB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/duckdb" CACHE PATH "Path to DuckDB source")

if(NOT EXISTS "${DUCKDB_SOURCE_DIR}/src/include/duckdb.hpp")
    message(FATAL_ERROR "DuckDB source not found at ${DUCKDB_SOURCE_DIR}. Run build.sh to fetch it.")
endif()

# Standalone builds own the DuckDB tree. An extension build may opt into the
# DuckDB targets already provided by its parent to avoid adding DuckDB twice.
if(DBSP_USE_PARENT_DUCKDB)
    if(NOT TARGET duckdb_static)
        message(FATAL_ERROR "DBSP_USE_PARENT_DUCKDB=ON requires a parent DuckDB build with target duckdb_static")
    endif()
elseif(NOT TARGET duckdb_static)
    set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHELL ON CACHE BOOL "" FORCE)
    set(BUILD_EXTENSIONS "" CACHE STRING "" FORCE)
    add_subdirectory(${DUCKDB_SOURCE_DIR} duckdb EXCLUDE_FROM_ALL)
endif()

# Extension source
# Extension build
set(DUCKDB_MODULE_BASE_DIR "${DUCKDB_SOURCE_DIR}")
find_package(Git REQUIRED)
execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${DUCKDB_SOURCE_DIR}" rev-parse --short=10 HEAD
    OUTPUT_VARIABLE DUCKDB_NORMALIZED_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE DUCKDB_GIT_HASH_RESULT
)
if(NOT DUCKDB_GIT_HASH_RESULT EQUAL 0 OR NOT DUCKDB_NORMALIZED_VERSION)
    message(FATAL_ERROR "Could not determine the DuckDB extension version from ${DUCKDB_SOURCE_DIR}")
endif()
set(DUCKDB_EXTENSION_DBSP_EXT_VERSION ${EXT_VERSION_DBSP})
set(PARAMETERS "-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS}
    src/dbsp_extension.cpp
    src/dbsp_recovery.cpp
    src/dbsp_crash_marker.cpp
    src/dbsp_wal_manager.cpp
)

target_include_directories(${TARGET_NAME}_loadable_extension PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${DUCKDB_SOURCE_DIR}/src/include
    ${DUCKDB_SOURCE_DIR}/extension/core_functions/include
    # DuckDB 2.0: core headers (cast_helpers.hpp) include "fmt/format.h".
    # DuckDB adds third_party/fmt/include at its own project scope, which does
    # not reach targets defined in this parent project.
    ${DUCKDB_SOURCE_DIR}/third_party/fmt/include
)

# The DuckDB tree in duckdb/ is STOCK — the extension carries no engine patch.
# Change capture comes from generated statement triggers (dbsp_trigger_source.hpp).

message(STATUS "")
message(STATUS "DBSP DuckDB Extension v${EXT_VERSION_DBSP}")
message(STATUS "  DuckDB: ${DUCKDB_SOURCE_DIR}")
message(STATUS "")

# Add tests
enable_testing()
add_subdirectory(test)

# Benchmark (only when the benchmark sources are present)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmark/benchmark_runner.cpp")
    add_executable(benchmark_runner
        benchmark/benchmark_runner.cpp
        src/dbsp_extension.cpp
        src/dbsp_recovery.cpp
        src/dbsp_crash_marker.cpp
        src/dbsp_wal_manager.cpp
    )
    target_include_directories(benchmark_runner PRIVATE
        ${DUCKDB_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(benchmark_runner duckdb_static
                          duckdb_generated_extension_loader
                          core_functions_extension parquet_extension)
endif()
