#pragma once
//
// app/tts.h — a procedural FORMANT speech synthesizer (M24), header-only and PURE.
//
// No assets, no recordings: speech is generated from formant resonators driven by a
// glottal source — the classic Klatt / eSpeak approach. Text -> phonemes (a small PA
// lexicon + a letter-to-sound fallback) -> formant tracks -> 16-bit PCM. Deterministic
// (a pure function of the text + sample rate). It is built to be read back by whisper.cpp,
// so the Backrooms PA gets a literal robotic VOICE the Shoggoth can hear as WORDS (M24) —
// not just the coarse sound tags of M23. A deep, slow, flat delivery: intelligible and
// fittingly inhuman for an intercom in the depths.
//
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace br::app {

namespace tts {

// A compact English phoneme set (ARPABET-ish). SIL = silence/pause.
enum class Ph {
    SIL,
    AA, AE, AH, AO, EH, ER, IH, IY, UH, UW, OW, EY, AY, AW, OY,  // vowels + diphthongs
    P, B, T, D, K, G,            // stops
    M, N, NG,                    // nasals
    F, V, S, Z, SH, ZH, TH, DH, HH,  // fricatives
    L, R, W, Y,                  // approximants
    CH, JH                       // affricates
};

// Per-phoneme synthesis spec: formant targets (Hz), voicing, frication, and timing.
struct Spec {
    float f1, f2, f3;   // formant centres (Hz)
    float dur;          // nominal duration (s)
    float amp;          // output gain
    bool voiced;        // glottal source on
    bool noise;         // frication / aspiration noise on
    float noise_hz;     // frication band centre (Hz)
    bool stop;          // a closure + burst
};

inline const Spec& spec_of(Ph p) {
    // Formants from standard male-voice vowel charts; consonants tuned for intelligibility.
    static const std::unordered_map<int, Spec> kT = {
        {(int)Ph::SIL, { 500, 1500, 2500, 0.090f, 0.0f, false, false, 0,    false}},
        // vowels (longer, full amplitude)
        {(int)Ph::AA,  { 730, 1090, 2440, 0.165f, 1.00f, true,  false, 0,    false}},
        {(int)Ph::AE,  { 660, 1720, 2410, 0.165f, 1.00f, true,  false, 0,    false}},
        {(int)Ph::AH,  { 640, 1190, 2390, 0.130f, 0.95f, true,  false, 0,    false}},
        {(int)Ph::AO,  { 570,  840, 2410, 0.165f, 1.00f, true,  false, 0,    false}},
        {(int)Ph::EH,  { 530, 1840, 2480, 0.150f, 1.00f, true,  false, 0,    false}},
        {(int)Ph::ER,  { 490, 1350, 1690, 0.155f, 0.95f, true,  false, 0,    false}},
        {(int)Ph::IH,  { 390, 1990, 2550, 0.130f, 0.95f, true,  false, 0,    false}},
        {(int)Ph::IY,  { 270, 2290, 3010, 0.160f, 1.00f, true,  false, 0,    false}},
        {(int)Ph::UH,  { 440, 1020, 2240, 0.130f, 0.95f, true,  false, 0,    false}},
        {(int)Ph::UW,  { 300,  870, 2240, 0.165f, 1.00f, true,  false, 0,    false}},
        {(int)Ph::OW,  { 430,  900, 2240, 0.180f, 1.00f, true,  false, 0,    false}},  // -> UW glide handled below
        {(int)Ph::EY,  { 400, 2100, 2600, 0.180f, 1.00f, true,  false, 0,    false}},  // -> IY
        {(int)Ph::AY,  { 730, 1200, 2440, 0.190f, 1.00f, true,  false, 0,    false}},  // AA -> IY
        {(int)Ph::AW,  { 730, 1090, 2440, 0.190f, 1.00f, true,  false, 0,    false}},  // AA -> UW
        {(int)Ph::OY,  { 570,  840, 2410, 0.190f, 1.00f, true,  false, 0,    false}},  // AO -> IY
        // stops: short closure + burst (formants steer the burst/transition)
        {(int)Ph::P,   { 400,  900, 2400, 0.090f, 0.7f,  false, true,  1800, true}},
        {(int)Ph::B,   { 350,  900, 2400, 0.080f, 0.7f,  true,  true,  1200, true}},
        {(int)Ph::T,   { 400, 1700, 2600, 0.090f, 0.8f,  false, true,  3800, true}},
        {(int)Ph::D,   { 350, 1700, 2600, 0.080f, 0.8f,  true,  true,  2600, true}},
        {(int)Ph::K,   { 400, 1900, 2400, 0.090f, 0.8f,  false, true,  2200, true}},
        {(int)Ph::G,   { 350, 1900, 2400, 0.080f, 0.8f,  true,  true,  1800, true}},
        // nasals: voiced, low first formant
        {(int)Ph::M,   { 250,  900, 2200, 0.110f, 0.7f,  true,  false, 0,    false}},
        {(int)Ph::N,   { 250, 1700, 2600, 0.110f, 0.7f,  true,  false, 0,    false}},
        {(int)Ph::NG,  { 250, 2000, 2600, 0.110f, 0.7f,  true,  false, 0,    false}},
        // fricatives: noise in a band; voiced ones add the glottal buzz
        {(int)Ph::F,   { 400, 1400, 2400, 0.120f, 0.55f, false, true,  4500, false}},
        {(int)Ph::V,   { 350, 1400, 2400, 0.110f, 0.55f, true,  true,  4500, false}},
        {(int)Ph::S,   { 400, 1700, 2600, 0.130f, 0.65f, false, true,  6500, false}},
        {(int)Ph::Z,   { 350, 1700, 2600, 0.120f, 0.65f, true,  true,  6000, false}},
        {(int)Ph::SH,  { 400, 2000, 2600, 0.140f, 0.70f, false, true,  3200, false}},
        {(int)Ph::ZH,  { 350, 2000, 2600, 0.120f, 0.70f, true,  true,  3000, false}},
        {(int)Ph::TH,  { 400, 1600, 2500, 0.110f, 0.45f, false, true,  5500, false}},
        {(int)Ph::DH,  { 350, 1600, 2500, 0.100f, 0.45f, true,  true,  5000, false}},
        {(int)Ph::HH,  { 500, 1500, 2500, 0.090f, 0.40f, false, true,  2000, false}},
        // approximants: voiced, formant-shaped
        {(int)Ph::L,   { 360, 1300, 2600, 0.110f, 0.85f, true,  false, 0,    false}},
        {(int)Ph::R,   { 420, 1300, 1600, 0.110f, 0.85f, true,  false, 0,    false}},
        {(int)Ph::W,   { 300,  800, 2200, 0.090f, 0.80f, true,  false, 0,    false}},
        {(int)Ph::Y,   { 270, 2200, 3000, 0.090f, 0.80f, true,  false, 0,    false}},
        // affricates: treated as a stop burst + fricative band
        {(int)Ph::CH,  { 400, 2000, 2600, 0.130f, 0.75f, false, true,  3200, true}},
        {(int)Ph::JH,  { 350, 2000, 2600, 0.120f, 0.75f, true,  true,  3000, true}},
    };
    auto it = kT.find((int)p);
    return it->second;
}

// Diphthongs glide from their start formants (in spec_of) toward an end target.
inline bool diphthong_end(Ph p, float& f1, float& f2, float& f3) {
    switch (p) {
        case Ph::OW: f1 = 300; f2 = 870;  f3 = 2240; return true;  // -> UW
        case Ph::EY: f1 = 270; f2 = 2290; f3 = 3010; return true;  // -> IY
        case Ph::AY: f1 = 270; f2 = 2290; f3 = 3010; return true;  // -> IY
        case Ph::AW: f1 = 300; f2 = 870;  f3 = 2240; return true;  // -> UW
        case Ph::OY: f1 = 270; f2 = 2290; f3 = 3010; return true;  // -> IY
        default: return false;
    }
}

// A 2-pole resonator (formant filter).
struct Reson {
    float b1 = 0, b2 = 0, a0 = 1, y1 = 0, y2 = 0;
    void set(float freq, float bw, float sr) {
        const float r = std::exp(-3.14159265f * bw / sr);
        const float theta = 2.0f * 3.14159265f * freq / sr;
        b1 = 2.0f * r * std::cos(theta);
        b2 = -r * r;
        a0 = 1.0f - b1 - b2;  // ~unity gain at DC; the peak rings at `freq`
    }
    float step(float x) {
        const float y = a0 * x + b1 * y1 + b2 * y2;
        y2 = y1; y1 = y;
        return y;
    }
};

// A tiny deterministic PRNG for the noise source (no <random>, no global state).
struct Noise {
    uint32_t s;
    explicit Noise(uint32_t seed) : s(seed ? seed : 0x1234567u) {}
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s / 2147483648.0f) - 1.0f; }
};

// --- G2P: a Backrooms-PA lexicon + a crude letter-to-sound fallback ----------------
inline const std::vector<Ph>* lexicon_lookup(const std::string& w) {
    using V = std::vector<Ph>;
    static const std::unordered_map<std::string, V> kLex = {
        {"WARNING",     {Ph::W, Ph::AO, Ph::R, Ph::N, Ph::IH, Ph::NG}},
        {"DANGER",      {Ph::D, Ph::EY, Ph::N, Ph::JH, Ph::ER}},
        {"ALERT",       {Ph::AH, Ph::L, Ph::ER, Ph::T}},
        {"LEVEL",       {Ph::L, Ph::EH, Ph::V, Ph::AH, Ph::L}},
        {"SECTOR",      {Ph::S, Ph::EH, Ph::K, Ph::T, Ph::ER}},
        {"CONTAINMENT", {Ph::K, Ph::AH, Ph::N, Ph::T, Ph::EY, Ph::N, Ph::M, Ph::AH, Ph::N, Ph::T}},
        {"BREACH",      {Ph::B, Ph::R, Ph::IY, Ph::CH}},
        {"EVACUATE",    {Ph::IH, Ph::V, Ph::AE, Ph::K, Ph::Y, Ph::UW, Ph::EY, Ph::T}},
        {"EXIT",        {Ph::EH, Ph::G, Ph::Z, Ph::IH, Ph::T}},
        {"BIOME",       {Ph::B, Ph::AY, Ph::OW, Ph::M}},
        {"LOOPING",     {Ph::L, Ph::UW, Ph::P, Ph::IH, Ph::NG}},
        {"DO",          {Ph::D, Ph::UW}},
        {"NOT",         {Ph::N, Ph::AA, Ph::T}},
        {"RUN",         {Ph::R, Ph::AH, Ph::N}},
        {"STAY",        {Ph::S, Ph::T, Ph::EY}},
        {"CALM",        {Ph::K, Ph::AA, Ph::M}},
        {"THE",         {Ph::DH, Ph::AH}},
        {"IS",          {Ph::IH, Ph::Z}},
        {"REMAIN",      {Ph::R, Ph::IY, Ph::M, Ph::EY, Ph::N}},
        {"INITIATE",    {Ph::IH, Ph::N, Ph::IH, Ph::SH, Ph::IY, Ph::EY, Ph::T}},
        {"ZERO",        {Ph::Z, Ph::IH, Ph::R, Ph::OW}},
        {"ONE",         {Ph::W, Ph::AH, Ph::N}},
        {"TWO",         {Ph::T, Ph::UW}},
        {"THREE",       {Ph::TH, Ph::R, Ph::IY}},
        {"FOUR",        {Ph::F, Ph::AO, Ph::R}},
        {"FIVE",        {Ph::F, Ph::AY, Ph::V}},
        {"SIX",         {Ph::S, Ph::IH, Ph::K, Ph::S}},
        {"SEVEN",       {Ph::S, Ph::EH, Ph::V, Ph::AH, Ph::N}},
        {"EIGHT",       {Ph::EY, Ph::T}},
        {"NINE",        {Ph::N, Ph::AY, Ph::N}},
    };
    auto it = kLex.find(w);
    return it == kLex.end() ? nullptr : &it->second;
}

// Crude single-letter fallback for words not in the lexicon (approximate, but keeps the
// synth speaking rather than silent on unexpected Director text).
inline void letters_to_phonemes(const std::string& w, std::vector<Ph>& out) {
    for (size_t i = 0; i < w.size(); ++i) {
        const char c = w[i];
        switch (c) {
            case 'A': out.push_back(Ph::AE); break;
            case 'B': out.push_back(Ph::B); break;
            case 'C': out.push_back(Ph::K); break;
            case 'D': out.push_back(Ph::D); break;
            case 'E': out.push_back(Ph::EH); break;
            case 'F': out.push_back(Ph::F); break;
            case 'G': out.push_back(Ph::G); break;
            case 'H': out.push_back(Ph::HH); break;
            case 'I': out.push_back(Ph::IH); break;
            case 'J': out.push_back(Ph::JH); break;
            case 'K': out.push_back(Ph::K); break;
            case 'L': out.push_back(Ph::L); break;
            case 'M': out.push_back(Ph::M); break;
            case 'N': out.push_back(Ph::N); break;
            case 'O': out.push_back(Ph::OW); break;
            case 'P': out.push_back(Ph::P); break;
            case 'Q': out.push_back(Ph::K); break;
            case 'R': out.push_back(Ph::R); break;
            case 'S': out.push_back(Ph::S); break;
            case 'T': out.push_back(Ph::T); break;
            case 'U': out.push_back(Ph::AH); break;
            case 'V': out.push_back(Ph::V); break;
            case 'W': out.push_back(Ph::W); break;
            case 'X': out.push_back(Ph::K); out.push_back(Ph::S); break;
            case 'Y': out.push_back(Ph::IY); break;
            case 'Z': out.push_back(Ph::Z); break;
            default: break;
        }
    }
}

// Text -> a phoneme stream (words separated by SIL pauses).
inline std::vector<Ph> text_to_phonemes(const std::string& text) {
    std::vector<Ph> ph;
    std::string word;
    auto flush = [&]() {
        if (word.empty()) return;
        const std::vector<Ph>* lx = lexicon_lookup(word);
        if (lx) ph.insert(ph.end(), lx->begin(), lx->end());
        else letters_to_phonemes(word, ph);
        ph.push_back(Ph::SIL);  // a short gap between words
        word.clear();
    };
    for (char c : text) {
        if ((c >= 'A' && c <= 'Z')) word.push_back(c);
        else if (c >= 'a' && c <= 'z') word.push_back(static_cast<char>(c - 'a' + 'A'));
        else flush();  // any non-letter ends the word
    }
    flush();
    return ph;
}

// Synthesize a phoneme stream to mono 16-bit PCM at `sr`. A deep PA delivery — but with
// real prosody (a declining pitch contour + jitter) and a sawtooth glottal source, so
// whisper hears SPEECH, not a sung monotone (a flat buzz reads as music).
inline std::vector<int16_t> synthesize_phonemes(const std::vector<Ph>& ph, uint32_t sr) {
    const float base_f0 = 105.0f;           // glottal pitch (Hz) — deep
    Reson r1, r2, r3;
    Noise noise(0xA11CE5u);
    std::vector<float> buf;
    float glottal_phase = 0.0f;
    float jitter = 0.0f;                     // refreshed once per glottal period
    float src_lp = 0.0f;                     // one-pole tilt on the voiced source
    float nlp = 0.0f;                        // one-pole state for the frication band
    float prev_f1 = 500, prev_f2 = 1500, prev_f3 = 2500;

    // Total voiced/spoken length, for the declination contour (pitch falls across the line).
    uint32_t total_n = 0;
    for (Ph p : ph) total_n += static_cast<uint32_t>(spec_of(p).dur * sr);
    if (total_n == 0) total_n = 1;
    uint32_t total_i = 0;

    for (size_t pi = 0; pi < ph.size(); ++pi) {
        const Ph p = ph[pi];
        const Spec& s = spec_of(p);
        const uint32_t n = static_cast<uint32_t>(s.dur * sr);
        float end_f1 = s.f1, end_f2 = s.f2, end_f3 = s.f3;
        diphthong_end(p, end_f1, end_f2, end_f3);
        const uint32_t closure = s.stop ? static_cast<uint32_t>(0.045f * sr) : 0u;  // stop silence

        for (uint32_t i = 0; i < n; ++i, ++total_i) {
            const float t = (n > 1) ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
            // glide the formants from the previous phoneme in, then toward the end target
            const float tin = t < 0.25f ? t / 0.25f : 1.0f;          // 25 ms-ish transition in
            const float cf1 = prev_f1 + (s.f1 - prev_f1) * tin;
            const float cf2 = prev_f2 + (s.f2 - prev_f2) * tin;
            const float cf3 = prev_f3 + (s.f3 - prev_f3) * tin;
            const float f1 = cf1 + (end_f1 - s.f1) * t;
            const float f2 = cf2 + (end_f2 - s.f2) * t;
            const float f3 = cf3 + (end_f3 - s.f3) * t;
            r1.set(f1, 90.0f, static_cast<float>(sr));
            r2.set(f2, 120.0f, static_cast<float>(sr));
            r3.set(f3, 180.0f, static_cast<float>(sr));

            // amplitude envelope: smooth raised-cosine attack/decay (no clicks)
            float env = 1.0f;
            const float edge = 0.14f;
            if (t < edge) env = 0.5f - 0.5f * std::cos(3.14159265f * t / edge);
            else if (t > 1.0f - edge) env = 0.5f - 0.5f * std::cos(3.14159265f * (1.0f - t) / edge);

            // pitch contour: declination across the utterance + a per-period jitter. A moving
            // pitch is what separates speech from a held (musical) note.
            const float prog = static_cast<float>(total_i) / static_cast<float>(total_n);
            const float f0 = base_f0 * (1.12f - 0.34f * prog) * (1.0f + 0.05f * jitter);

            // for a stop, hold silence through the closure, then a burst
            const bool in_closure = s.stop && (i < closure);
            float src = 0.0f;
            if (!in_closure) {
                if (s.voiced) {
                    glottal_phase += f0 / static_cast<float>(sr);
                    if (glottal_phase >= 1.0f) { glottal_phase -= 1.0f; jitter = noise.next(); }
                    const float saw = 2.0f * glottal_phase - 1.0f;  // sawtooth: 1/n rolloff, less buzzy
                    src_lp += 0.35f * (saw - src_lp);               // mild spectral tilt (warmer voice)
                    src += src_lp;
                    src += 0.015f * noise.next();                   // a touch of aspiration
                }
                if (s.noise) {
                    const float ns = noise.next();
                    const float k = std::min(1.0f, s.noise_hz / (static_cast<float>(sr) * 0.5f));
                    nlp += k * (ns - nlp);
                    const float band = ns - nlp * 0.5f;  // emphasise the high band
                    const float burst = (s.stop && i >= closure && i < closure + static_cast<uint32_t>(0.012f * sr)) ? 2.4f : 1.0f;
                    src += band * 0.9f * burst;
                }
            }
            float v = r3.step(r2.step(r1.step(src)));
            v *= s.amp * env;
            buf.push_back(v);
        }
        prev_f1 = end_f1; prev_f2 = end_f2; prev_f3 = end_f3;
    }

    // normalise to a comfortable peak, then to PCM16
    float peak = 1e-6f;
    for (float v : buf) peak = std::max(peak, std::fabs(v));
    const float g = 0.85f / peak;
    std::vector<int16_t> pcm;
    pcm.reserve(buf.size());
    for (float v : buf) {
        float x = v * g;
        x = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
        pcm.push_back(static_cast<int16_t>(x * 32767.0f));
    }
    return pcm;
}

}  // namespace tts

// Public: synthesize `text` to mono 16-bit PCM at `sr`. Deterministic, no assets.
inline std::vector<int16_t> synthesize_speech(const std::string& text, uint32_t sr) {
    return tts::synthesize_phonemes(tts::text_to_phonemes(text), sr);
}

}  // namespace br::app
