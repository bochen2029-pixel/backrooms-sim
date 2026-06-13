# gen — L1 procedural generation. Deterministic (INV-2): compiled /fp:strict.
# M3 adds GenerateChunk (chunk.cpp) implementing the chunk_gen_v1 contract.
add_library(gen STATIC
  src/gen.cpp
  src/chunk.cpp)
target_include_directories(gen PUBLIC include)
target_link_libraries(gen
  PUBLIC core contracts
  PRIVATE backrooms_sim_flags)
add_library(backrooms::gen ALIAS gen)
