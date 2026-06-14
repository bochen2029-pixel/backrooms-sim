# telemetry — L2. Counters + CSV (telemetry_v1). M3 adds the frame CSV writer;
# M10 adds minidump capture (crash.cpp -> dbghelp, system import lib).
add_library(telemetry STATIC
  src/telemetry.cpp
  src/csv.cpp
  src/crash.cpp)
target_include_directories(telemetry PUBLIC include)
target_link_libraries(telemetry
  PUBLIC contracts
  PRIVATE backrooms_flags dbghelp)
add_library(backrooms::telemetry ALIAS telemetry)
