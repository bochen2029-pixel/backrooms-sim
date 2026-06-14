# app — L2 composition root. Depends on ALL modules (ARCHITECTURE.md §4).
# Nothing depends on app. M1: drives the renderer (headless/windowed), writes
# PNGs via the shared stb wrapper, and creates the Win32 window (user32).
# M16: xinput (gamepad) is a Windows system import lib — no vcpkg change (like winmm).
add_executable(backrooms src/main.cpp src/hud.cpp)
target_link_libraries(backrooms PRIVATE
  backrooms_flags
  core gen stream telemetry audio render_d3d12 render_dxr director
  br_stb user32 winmm xinput)
target_compile_definitions(backrooms PRIVATE UNICODE _UNICODE)
set_target_properties(backrooms PROPERTIES OUTPUT_NAME "backrooms")
