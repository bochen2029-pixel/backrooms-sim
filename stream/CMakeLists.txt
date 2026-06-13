# stream — L2 chunk ring + worker pool. Depends on core + gen (+ contracts).
# M3 adds StreamManager (background generation, bounded residency).
add_library(stream STATIC
  src/stream.cpp
  src/stream_manager.cpp)
target_include_directories(stream PUBLIC include)
target_link_libraries(stream
  PUBLIC core gen contracts
  PRIVATE backrooms_flags)
add_library(backrooms::stream ALIAS stream)
