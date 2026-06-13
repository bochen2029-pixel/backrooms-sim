cmake_minimum_required(VERSION 3.28)

project(backrooms_sim
  VERSION 0.0.0
  DESCRIPTION "Infinite procedurally generated Backrooms walking simulation"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global standards (CLAUDE.md Code standards: C++20, MSVC, warnings-as-errors)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Static CRT to match the x64-windows-static vcpkg triplet (self-contained exes).
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Deterministic, script-discoverable output locations.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

enable_testing()
include(CTest)

# ---------------------------------------------------------------------------
# backrooms_flags : strict warning/standard flags applied to OUR code only.
# Third-party (<...>) headers are treated as external and never warn (/WX safe).
# ---------------------------------------------------------------------------
add_library(backrooms_flags INTERFACE)
target_compile_features(backrooms_flags INTERFACE cxx_std_20)
if(MSVC)
  target_compile_options(backrooms_flags INTERFACE
    /W4 /WX /permissive- /EHsc /utf-8
    /external:anglebrackets /external:W0)
  target_compile_definitions(backrooms_flags INTERFACE
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# backrooms_sim_flags : the above plus /fp:strict for the deterministic sim
# core and anything that must honour INV-1 (ARCHITECTURE.md ADR-003).
add_library(backrooms_sim_flags INTERFACE)
target_link_libraries(backrooms_sim_flags INTERFACE backrooms_flags)
if(MSVC)
  target_compile_options(backrooms_sim_flags INTERFACE /fp:strict)
endif()

# ---------------------------------------------------------------------------
# contracts — boundary headers (ARCHITECTURE.md §5), included as
# "contracts/<name>.h". Header-only INTERFACE target linked by both the producer
# and the consumer of each boundary.
# ---------------------------------------------------------------------------
add_library(contracts INTERFACE)
target_include_directories(contracts INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# backrooms_module : declare a stub static-library module with the right flags
# and dependency arrows. SIM => compile /fp:strict (deterministic modules).
# Each module owns its include/<name>/<name>.h and src/<name>.cpp at M0.
# ---------------------------------------------------------------------------
function(backrooms_module name)
  cmake_parse_arguments(M "SIM" "" "DEPS" ${ARGN})
  add_library(${name} STATIC src/${name}.cpp)
  target_include_directories(${name} PUBLIC include)
  if(M_SIM)
    target_link_libraries(${name} PRIVATE backrooms_sim_flags)
  else()
    target_link_libraries(${name} PRIVATE backrooms_flags)
  endif()
  if(M_DEPS)
    target_link_libraries(${name} PUBLIC ${M_DEPS})
  endif()
  add_library(backrooms::${name} ALIAS ${name})
endfunction()

# ---------------------------------------------------------------------------
# Modules. Added in DAG order; `core` depends on nothing (ARCHITECTURE.md §4).
# ---------------------------------------------------------------------------
add_subdirectory(core)
add_subdirectory(telemetry)
add_subdirectory(gen)
add_subdirectory(stream)
add_subdirectory(audio)
add_subdirectory(render_d3d12)
add_subdirectory(render_dxr)
add_subdirectory(director)
add_subdirectory(app)
add_subdirectory(tools)
add_subdirectory(tests)
