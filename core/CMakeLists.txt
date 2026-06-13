# core — L1 deterministic sim. Depends on NOTHING (ARCHITECTURE.md §4).
# Compiled with /fp:strict via backrooms_sim_flags (ADR-003).
add_library(core STATIC
  src/version.cpp)

target_include_directories(core PUBLIC include)
target_link_libraries(core PRIVATE backrooms_sim_flags)

add_library(backrooms::core ALIAS core)
