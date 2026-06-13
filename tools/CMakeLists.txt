# tools — dev executables (hashdiff, goldgen now; walkbot/contactsheet/wavcheck
# arrive with their milestones). Built against backrooms_flags. Third-party
# image I/O (stb) is isolated in br_stb so its warnings never trip /WX.
find_path(STB_INCLUDE_DIRS "stb_image.h" REQUIRED)

add_library(br_stb STATIC common/stb_impl.cpp)
target_include_directories(br_stb SYSTEM PUBLIC ${STB_INCLUDE_DIRS})
if(MSVC)
  target_compile_options(br_stb PRIVATE /w)  # third-party impl: silence warnings
endif()

add_executable(hashdiff hashdiff/main.cpp)
target_link_libraries(hashdiff PRIVATE backrooms_flags br_stb)

add_executable(goldgen goldgen/main.cpp)
target_link_libraries(goldgen PRIVATE backrooms_flags br_stb core)

# wavcheck (M6): WAV spectral gate. Uses the header-only audio/wav.h reader.
add_executable(wavcheck wavcheck/main.cpp)
target_link_libraries(wavcheck PRIVATE backrooms_flags audio)
