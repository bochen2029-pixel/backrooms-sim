# tests — Catch2 under CTest (ADR-008). Single `ctest` entry point for gates.
find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# --- Real unit suite (must always be green) ---
add_executable(unit_tests
  unit/test_rng.cpp
  unit/test_smoke.cpp)
target_link_libraries(unit_tests PRIVATE
  backrooms_flags
  core gen stream telemetry audio render_d3d12 render_dxr director
  Catch2::Catch2WithMain)
catch_discover_tests(unit_tests DISCOVERY_MODE PRE_TEST)

# --- Gate canary: deliberately failing, DISABLED so it never runs in the
# normal `ctest` pass. scripts/gate.ps1 invokes the binary directly to prove the
# harness reports failures as nonzero (M0 "test the gate itself"). ---
add_executable(gate_canary gate/test_canary.cpp)
target_link_libraries(gate_canary PRIVATE backrooms_flags Catch2::Catch2WithMain)
add_test(NAME gate_canary COMMAND gate_canary)
set_tests_properties(gate_canary PROPERTIES DISABLED TRUE)
