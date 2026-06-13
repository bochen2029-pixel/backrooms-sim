#pragma once
// audio/audio.h — procedural hum/drone synthesis, footsteps, raycast room-probe
// reverb, offline WAV render. M0 stub: identity only. Consumes core events via
// contracts/audio_events_v1.h starting M6.
namespace br::audio {
const char* module_name() noexcept;
}  // namespace br::audio
