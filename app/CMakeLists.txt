# app — L2 composition root. Depends on ALL modules (ARCHITECTURE.md §4).
# Nothing depends on app.
add_executable(backrooms src/main.cpp)
target_link_libraries(backrooms PRIVATE
  backrooms_flags
  core gen stream telemetry audio render_d3d12 render_dxr director)
set_target_properties(backrooms PROPERTIES OUTPUT_NAME "backrooms")
