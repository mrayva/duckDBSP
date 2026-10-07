// Catch main - provides main() automatically
#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "verify_vectors.hpp"

namespace {

// Every test binary links catch2_main, so arming here covers the whole suite —
// including the binaries that never construct a DuckDBTestHarness (unit tests
// and the crash-recovery integration tests open `DuckDB db(nullptr)`
// directly). Running before main() is safe: the target is
// DBConfigOptions::global_verification_mode, whose own initializer is a
// constant expression (config.cpp:28), so it is initialized before any dynamic
// initialization in any translation unit. Nothing in DuckDB resets it on
// database open — only an explicit RESET does
// (DebugVerificationModeSetting::ResetGlobal).
struct ArmVerifyVectorsAtStartup {
  ArmVerifyVectorsAtStartup() { dbsp_test::ArmVerifyVectorsIfRequested(); }
};
const ArmVerifyVectorsAtStartup arm_verify_vectors_at_startup;

} // namespace
