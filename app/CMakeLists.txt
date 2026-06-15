# app — L2 composition root. Depends on ALL modules (ARCHITECTURE.md §4).
# Nothing depends on app. M1: drives the renderer (headless/windowed), writes
# PNGs via the shared stb wrapper, and creates the Win32 window (user32).
# M16: xinput (gamepad) is a Windows system import lib — no vcpkg change (like winmm).
# M17: version.rc embeds text-only VERSIONINFO (no asset files) via rc.exe, and the
# app icon is generated procedurally at build time by `icongen` (still no committed
# asset — backrooms.ico is gitignored). version.rc depends on it so the rc recompiles.
if(WIN32)
  enable_language(RC)
  set(BR_ICON "${CMAKE_CURRENT_SOURCE_DIR}/backrooms.ico")
  add_custom_command(OUTPUT ${BR_ICON}
    COMMAND $<TARGET_FILE:icongen> ${BR_ICON}
    DEPENDS icongen
    COMMENT "icongen: procedural app icon -> backrooms.ico"
    VERBATIM)
  add_custom_target(gen_app_icon DEPENDS ${BR_ICON})
  set_source_files_properties(version.rc PROPERTIES OBJECT_DEPENDS ${BR_ICON})
endif()
add_executable(backrooms src/main.cpp src/hud.cpp version.rc)
if(WIN32)
  add_dependencies(backrooms gen_app_icon)
endif()
target_link_libraries(backrooms PRIVATE
  backrooms_flags
  core gen stream telemetry audio render_d3d12 render_dxr director
  br_stb user32 winmm xinput)
target_compile_definitions(backrooms PRIVATE UNICODE _UNICODE)
set_target_properties(backrooms PROPERTIES OUTPUT_NAME "backrooms")

# Windows screensaver: a .scr IS just the exe renamed (it self-detects /s /p /c). Emit Backrooms.scr
# next to the exe on EVERY build so it is reproducible -- it used to be a one-off manual copy, so a
# clean build (which wipes build/bin) had nothing to regenerate it and it vanished.
if(WIN32)
  add_custom_command(TARGET backrooms POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:backrooms> $<TARGET_FILE_DIR:backrooms>/Backrooms.scr
    COMMENT "screensaver: backrooms.exe -> Backrooms.scr"
    VERBATIM)
endif()
