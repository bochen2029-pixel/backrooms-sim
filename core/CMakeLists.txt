# core — L1 deterministic sim. Depends only on contract headers
# (ARCHITECTURE.md §4: core depends on nothing at the module level).
# Compiled with /fp:strict via backrooms_sim_flags (ADR-003).
add_library(core STATIC
  src/version.cpp
  src/world.cpp
  src/replay.cpp)

target_include_directories(core PUBLIC include)
target_link_libraries(core
  PUBLIC contracts
  PRIVATE backrooms_sim_flags)

add_library(backrooms::core ALIAS core)
