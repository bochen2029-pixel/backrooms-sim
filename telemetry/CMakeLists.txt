# telemetry — L2. Counters + CSV (telemetry_v1). M3 adds the frame CSV writer.
add_library(telemetry STATIC
  src/telemetry.cpp
  src/csv.cpp)
target_include_directories(telemetry PUBLIC include)
target_link_libraries(telemetry
  PUBLIC contracts
  PRIVATE backrooms_flags)
add_library(backrooms::telemetry ALIAS telemetry)
