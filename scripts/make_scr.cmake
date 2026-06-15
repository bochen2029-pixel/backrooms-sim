# Copy the freshly-built exe to Backrooms.scr (a Windows screensaver IS just the exe renamed; it
# self-detects /s /p /c). NON-FATAL by design: the destination may be LOCKED because the screensaver
# is currently running -- in that case we keep the running copy and simply skip, and the next build
# while it is closed refreshes it. The copy result is intentionally ignored so a locked .scr can never
# fail the build. Invoked from app/CMakeLists.txt with -DSRC=<exe> -DDST=<.scr> -P this-file.
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy "${SRC}" "${DST}"
  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
# rc deliberately not checked -> always succeed.
