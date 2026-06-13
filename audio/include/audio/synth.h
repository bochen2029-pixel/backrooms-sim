#pragma once
//
// audio/synth.h — deterministic procedural sound for the backrooms bed.
//
// A block-based synth: a fluorescent mains hum (60 Hz + harmonics), a low HVAC
// drone, footstep transients, and a Schroeder/Freeverb-style reverb whose decay
// is set by the room probe. Given the same seed and the same call sequence
// (set_reverb / trigger_footstep / render), output is bit-identical — so the
// offline WAV is reproducible and gate-checkable (INV-1/INV-7). No wall-clock.
//
#include <cstdint>
#include <vector>

#include "contracts/audio_events_v1.h"
#include "core/rng.h"

namespace br::audio {

class Synth {
public:
    Synth(uint64_t seed, uint32_t sample_rate);

    // Set the reverb decay (RT60 seconds) from the room probe.
    void set_reverb_seconds(float rt60);

    // Fire a footstep transient (intensity 0..1).
    void trigger_footstep(float intensity);

    // Render `frames` interleaved stereo float samples (writes 2*frames floats).
    void render(const contracts::AudioListener& listener, float* out, uint32_t frames);

private:
    // One-pole-damped feedback comb (Freeverb-style).
    struct Comb {
        std::vector<float> buf;
        size_t idx = 0;
        float store = 0.0f;
        float feedback = 0.0f;
        float damp = 0.25f;
        float process(float x) {
            float y = buf[idx];
            store = y * (1.0f - damp) + store * damp;
            buf[idx] = x + store * feedback;
            if (++idx >= buf.size()) idx = 0;
            return y;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        size_t idx = 0;
        float g = 0.5f;
        float process(float x) {
            float y = buf[idx];
            buf[idx] = x + y * g;
            if (++idx >= buf.size()) idx = 0;
            return y - x * g;
        }
    };
    struct Footstep {
        float amp = 0.0f;     // current envelope amplitude (0 = inactive)
        float decay = 0.0f;   // per-sample multiplicative decay
        float lp = 0.0f;      // one-pole lowpass state (thud body)
    };

    float white();  // deterministic noise in [-1,1)

    uint32_t sr_;
    core::Pcg64 rng_;

    double hum_phase_[4] = {0, 0, 0, 0};
    double lfo_phase_ = 0.0;
    float hvac_lp_ = 0.0f;

    Comb combL_[4], combR_[4];
    Allpass apL_[2], apR_[2];
    float reverb_seconds_ = 0.5f;

    static constexpr int kVoices = 8;
    Footstep voices_[kVoices];
};

}  // namespace br::audio
