#pragma once
//
// contracts/audio_events_v1.h — core -> audio boundary (v1.0).
//
// The sim core is the determinism oracle for sound too: audio is a pure function
// of (WorldSeed, input stream) just like the WorldState (INV-1). This header
// carries the two things the audio module needs from a tick — the listener pose
// and the discrete footfall events — both derived deterministically in `core`
// from the already-hashed WorldState (odometer + tick). No audio-module type
// ever crosses back into `core` (INV-5).
//
#include <cstdint>

namespace br::contracts {

// Output format of the offline render + real-time mixer (fixed; part of the
// determinism contract, so an offline WAV is bit-identical across runs).
constexpr uint32_t kAudioSampleRate = 48000;
constexpr uint32_t kAudioChannels = 2;       // interleaved L,R

// Meters of horizontal travel per footfall. Chosen so the wanderer's footstep
// cadence at kWalkSpeed reads as a brisk walk (~2.5 steps/s), not a sprint.
constexpr float kStrideLength = 1.6f;

// The listener (= the wanderer's ear) sampled at an audio block boundary.
struct AudioListener {
    float pos[3];   // world-space ear position (eye height)
    float yaw;      // radians; 0 looks toward +Z (for stereo panning)
    float speed;    // horizontal m/s (footstep loudness + bed modulation)
};

// A footfall: emitted on the tick where the integer footstep count increments.
struct FootstepEvent {
    uint64_t tick;     // sim tick of the footfall (aligns 1:1 with the replay)
    float intensity;   // 0..1, scales with walking speed at the footfall
};

}  // namespace br::contracts
