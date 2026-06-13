#include "audio/synth.h"

#include <cmath>

namespace br::audio {

namespace {
constexpr double kTwoPi = 6.283185307179586;

// Freeverb comb/allpass delay lengths (samples @44.1 kHz), scaled to our rate.
const int kCombLen[4] = {1557, 1617, 1491, 1422};
const int kApLen[2]   = {556, 441};
constexpr int kStereoSpread = 23;  // R delays offset for width

int scaled(int len44, uint32_t sr) {
    long v = static_cast<long>(static_cast<double>(len44) * static_cast<double>(sr) / 44100.0);
    return v < 1 ? 1 : static_cast<int>(v);
}
}  // namespace

Synth::Synth(uint64_t seed, uint32_t sample_rate) : sr_(sample_rate), rng_(seed) {
    for (int i = 0; i < 4; ++i) {
        combL_[i].buf.assign(static_cast<size_t>(scaled(kCombLen[i], sr_)), 0.0f);
        combR_[i].buf.assign(static_cast<size_t>(scaled(kCombLen[i] + kStereoSpread, sr_)), 0.0f);
        combL_[i].damp = combR_[i].damp = 0.30f;
    }
    for (int i = 0; i < 2; ++i) {
        apL_[i].buf.assign(static_cast<size_t>(scaled(kApLen[i], sr_)), 0.0f);
        apR_[i].buf.assign(static_cast<size_t>(scaled(kApLen[i] + kStereoSpread, sr_)), 0.0f);
        apL_[i].g = apR_[i].g = 0.5f;
    }
    set_reverb_seconds(reverb_seconds_);
}

float Synth::white() {
    return static_cast<float>(rng_.next_double()) * 2.0f - 1.0f;  // [-1,1)
}

void Synth::set_reverb_seconds(float rt60) {
    reverb_seconds_ = rt60;
    // Comb feedback for an RT60 decay: g = 10^(-3 * delaySeconds / rt60).
    for (int i = 0; i < 4; ++i) {
        const float dL = static_cast<float>(combL_[i].buf.size()) / static_cast<float>(sr_);
        const float dR = static_cast<float>(combR_[i].buf.size()) / static_cast<float>(sr_);
        float gL = std::pow(10.0f, -3.0f * dL / rt60);
        float gR = std::pow(10.0f, -3.0f * dR / rt60);
        if (gL > 0.98f) gL = 0.98f;
        if (gR > 0.98f) gR = 0.98f;
        combL_[i].feedback = gL;
        combR_[i].feedback = gR;
    }
}

void Synth::trigger_footstep(float intensity) {
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    // Find a free (or the quietest) voice.
    int slot = 0;
    float quietest = voices_[0].amp;
    for (int i = 0; i < kVoices; ++i) {
        if (voices_[i].amp <= 0.0f) { slot = i; quietest = -1.0f; break; }
        if (voices_[i].amp < quietest) { quietest = voices_[i].amp; slot = i; }
    }
    Footstep& v = voices_[slot];
    v.amp = 0.25f + 0.45f * intensity;
    v.decay = std::exp(-1.0f / (0.055f * static_cast<float>(sr_)));  // ~55 ms tail
    v.lp = 0.0f;
}

void Synth::render(const contracts::AudioListener& listener, float* out, uint32_t frames) {
    // Hum partials: 60 Hz fundamental + harmonics (the fluorescent ballast).
    const double f0 = 60.0;
    const float partial_amp[4] = {0.32f, 0.16f, 0.09f, 0.05f};
    const float hvac_coef = std::exp(-2.0f * 3.14159265f * 110.0f / static_cast<float>(sr_));
    const float foot_lp = std::exp(-2.0f * 3.14159265f * 900.0f / static_cast<float>(sr_));
    // Footstep loudness eases off when the wanderer is still.
    const float move = (listener.speed > 0.1f) ? 1.0f : 0.4f;

    for (uint32_t i = 0; i < frames; ++i) {
        // Fluorescent hum (slow amplitude flutter via a ~0.17 Hz LFO).
        const float lfo = 0.88f + 0.12f * static_cast<float>(std::sin(lfo_phase_));
        float hum = 0.0f;
        for (int p = 0; p < 4; ++p) {
            hum += partial_amp[p] * static_cast<float>(std::sin(hum_phase_[p]));
            hum_phase_[p] += kTwoPi * (f0 * (p + 1)) / static_cast<double>(sr_);
            if (hum_phase_[p] > kTwoPi) hum_phase_[p] -= kTwoPi;
        }
        hum *= 0.7f * lfo;
        lfo_phase_ += kTwoPi * 0.17 / static_cast<double>(sr_);
        if (lfo_phase_ > kTwoPi) lfo_phase_ -= kTwoPi;

        // HVAC drone: one-pole lowpassed white noise (a low rumble bed).
        hvac_lp_ = hvac_lp_ * hvac_coef + white() * (1.0f - hvac_coef);
        const float hvac = hvac_lp_ * 0.55f;

        // Footsteps: decaying lowpassed noise transients.
        float foot = 0.0f;
        for (int v = 0; v < kVoices; ++v) {
            Footstep& fs = voices_[v];
            if (fs.amp <= 0.0001f) { fs.amp = 0.0f; continue; }
            fs.lp = fs.lp * foot_lp + white() * (1.0f - foot_lp);
            foot += fs.lp * fs.amp;
            fs.amp *= fs.decay;
        }
        foot *= 1.6f * move;

        const float dry = hum + hvac + foot;
        const float send = hum * 0.25f + foot * 0.9f;  // bed is mostly dry

        float wetL = 0.0f, wetR = 0.0f;
        for (int c = 0; c < 4; ++c) { wetL += combL_[c].process(send); wetR += combR_[c].process(send); }
        wetL *= 0.25f; wetR *= 0.25f;
        for (int a = 0; a < 2; ++a) { wetL = apL_[a].process(wetL); wetR = apR_[a].process(wetR); }

        const float wet = 0.35f;
        out[2 * i + 0] = dry + wetL * wet;
        out[2 * i + 1] = dry + wetR * wet;
    }
}

}  // namespace br::audio
