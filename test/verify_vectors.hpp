// Suite-wide arming of DuckDB's vector verification.
//
// Set DBSP_TEST_VERIFY_VECTORS=1 in the environment and every test binary in
// this suite runs with debug_verification_mode='verify_vectors': DataChunk
// checks, at every operator boundary, that each of its child vectors reports
// the same size as the chunk (DataChunk::VerifyInternal,
// duckdb/src/common/types/data_chunk.cpp:458). A chunk produced with the
// deprecated SetCardinality leaves its children at size 0, and under this mode
// the engine throws
//   "DataChunk::Verify - size mismatch: vector N has size X but chunk has size Y"
// instead of quietly returning wrong answers. That failure mode is exactly the
// bug the DuckDB 2.0 migration hit (see the law at the top of
// src/dbsp_extension.cpp and the CHANGELOG entry); this switch makes the next
// occurrence loud everywhere rather than only in the one test that arms the
// mode by hand.
//
// CI should run the suite twice: `ctest` and `DBSP_TEST_VERIFY_VECTORS=1
// ctest`. There is deliberately no ctest label for it — the switch is an
// environment variable precisely so the second run needs no separate test
// registration.
//
// Why the static assignment rather than `SET GLOBAL debug_verification_mode`:
// it is the same variable the SQL setting writes
// (DebugVerificationModeSetting::SetGlobal, custom_settings.cpp:436) — the
// mode lives in DBConfigOptions::global_verification_mode, a PROCESS-wide
// static (config.cpp:28), not in any DatabaseInstance. Assigning it needs no
// Connection, so it also covers the many test binaries that open a raw
// `DuckDB db(nullptr)` and never build a DuckDBTestHarness.
#pragma once

#include "duckdb/main/config.hpp"

#include <cstdlib>
#include <cstring>

namespace dbsp_test {

// True when DBSP_TEST_VERIFY_VECTORS is set to anything but "" or "0".
// Read from the environment once per process.
inline bool VerifyVectorsRequested() {
  static const bool requested = [] {
    const char *flag = std::getenv("DBSP_TEST_VERIFY_VECTORS");
    return flag != nullptr && flag[0] != '\0' && std::strcmp(flag, "0") != 0;
  }();
  return requested;
}

// Arm the mode if requested. Idempotent, and cheap enough to call on every
// database open: the destination is one process-wide static.
inline void ArmVerifyVectorsIfRequested() {
  if (VerifyVectorsRequested()) {
    duckdb::DBConfigOptions::global_verification_mode =
        duckdb::DebugVerificationMode::VERIFY_VECTORS;
  }
}

} // namespace dbsp_test
