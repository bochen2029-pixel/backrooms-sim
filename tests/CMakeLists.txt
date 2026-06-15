# tests — Catch2 under CTest (ADR-008). Single `ctest` entry point for gates.
find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# --- Real unit suite (must always be green) ---
add_executable(unit_tests
  unit/test_rng.cpp
  unit/test_smoke.cpp
  unit/test_collision.cpp
  unit/test_sim.cpp
  unit/test_chunk.cpp
  unit/test_stream.cpp
  unit/test_layout.cpp
  unit/test_texgen.cpp
  unit/test_audio.cpp
  unit/test_biome.cpp
  unit/test_director.cpp
  unit/test_menu.cpp
  unit/test_config.cpp
  unit/test_gamepad.cpp
  unit/test_headbob.cpp
  unit/test_shoggoth.cpp)
target_link_libraries(unit_tests PRIVATE
  backrooms_flags
  core gen stream telemetry audio render_d3d12 render_dxr director
  Catch2::Catch2WithMain)
# test_menu.cpp includes the header-only game-state machine (app/src/menu.h); the
# app shell is an executable, so expose just its source dir for that one header.
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/app/src)
catch_discover_tests(unit_tests DISCOVERY_MODE PRE_TEST)

# --- Gate canary: deliberately failing, DISABLED so it never runs in the
# normal `ctest` pass. scripts/gate.ps1 invokes the binary directly to prove the
# harness reports failures as nonzero (M0 "test the gate itself"). ---
add_executable(gate_canary gate/test_canary.cpp)
target_link_libraries(gate_canary PRIVATE backrooms_flags Catch2::Catch2WithMain)
add_test(NAME gate_canary COMMAND gate_canary)
set_tests_properties(gate_canary PROPERTIES DISABLED TRUE)
