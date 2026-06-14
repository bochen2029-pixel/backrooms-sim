# audio — L2 procedural audio. Consumes core events.
backrooms_module(audio DEPS core)

# M6: procedural synth + room-probe reverb. WAV I/O is header-only (audio/wav.h).
# M14: device.cpp = real-time miniaudio playback backend (ADR-040). miniaudio is
# header-only; its implementation is compiled into device.cpp and consumed as
# <miniaudio.h> (external → /external:W0, so /WX is unaffected).
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h")

target_sources(audio PRIVATE
  src/synth.cpp
  src/room_probe.cpp
  src/engine.cpp
  src/device.cpp)

target_include_directories(audio PRIVATE ${MINIAUDIO_INCLUDE_DIRS})
