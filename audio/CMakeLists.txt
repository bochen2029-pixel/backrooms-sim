# audio — L2 procedural audio. Consumes core events.
backrooms_module(audio DEPS core)

# M6: procedural synth + room-probe reverb. WAV I/O is header-only (audio/wav.h).
# render.cpp (offline) + engine.cpp (real-time) arrive in the later M6 steps.
target_sources(audio PRIVATE
  src/synth.cpp
  src/room_probe.cpp
  src/engine.cpp)
