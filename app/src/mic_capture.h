#pragma once
//
// app/mic_capture.h — live microphone capture + voice-activity detection (VAD).
//
// The two-way Director (ADR-074) lets the wanderer TALK to the facility intelligence. This
// is the input half: a continuous 16 kHz mono PCM16 capture (winmm waveIn, polled -- no
// callback thread) with a simple energy-based VAD that segments a spoken UTTERANCE (speech,
// then ~0.7 s of trailing silence). `poll()` each frame; it returns the utterance's PCM the
// moment it ends, ready to write as a WAV for whisper.cpp.
//
// HALF-DUPLEX ECHO CONTROL (no acoustic echo cancellation needed): `suspend_until(ms)` gates
// capture OFF while the Director's PA voice is playing (+ a short tail), so the facility never
// hears itself through the speakers and transcribe-loops. The frame loop syncs the global PA
// suspend window in each frame before polling. Graceful: if there is no mic / waveInOpen fails,
// `start()` returns false and `poll()` is a no-op -- the game runs fine without voice.
//
// Pure capture + a tiny DSP state machine: no renderer, no network, no game state. Live
// presentation only (never touches the sim/replay/goldens) -- like the live Director/brain.
//
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace br::app {

class MicCapture {
public:
    static constexpr uint32_t kRate = 16000;          // whisper wants 16 kHz mono
    MicCapture() = default;
    ~MicCapture() { stop(); }
    MicCapture(const MicCapture&) = delete;
    MicCapture& operator=(const MicCapture&) = delete;

    // Open + start the default capture device. Returns false (graceful) if none / it fails.
    bool start() {
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 1;
        wfx.nSamplesPerSec = kRate;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 2;
        wfx.nAvgBytesPerSec = kRate * 2u;
        if (waveInOpen(&hwi_, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            hwi_ = nullptr;
            return false;
        }
        for (int i = 0; i < kBufs; ++i) {
            bufmem_[i].assign(kBufSamples, 0);
            hdr_[i] = WAVEHDR{};
            hdr_[i].lpData = reinterpret_cast<LPSTR>(bufmem_[i].data());
            hdr_[i].dwBufferLength = kBufSamples * 2u;
            if (waveInPrepareHeader(hwi_, &hdr_[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) { stop(); return false; }
            waveInAddBuffer(hwi_, &hdr_[i], sizeof(WAVEHDR));
        }
        if (waveInStart(hwi_) != MMSYSERR_NOERROR) { stop(); return false; }
        return true;
    }

    void stop() {
        if (!hwi_) return;
        waveInReset(hwi_);
        for (int i = 0; i < kBufs; ++i)
            if (hdr_[i].dwFlags & WHDR_PREPARED) waveInUnprepareHeader(hwi_, &hdr_[i], sizeof(WAVEHDR));
        waveInClose(hwi_);
        hwi_ = nullptr;
    }

    bool active() const { return hwi_ != nullptr; }
    bool listening() const { return speaking_; }          // a UI hint: speech currently being captured

    // Gate capture off until GetTickCount64() reaches `until_ms` (the PA-voice echo window).
    void suspend_until(uint64_t until_ms) { suspend_until_ms_ = until_ms; }

    // Non-blocking. Drains ready capture buffers, runs the VAD, and recycles the buffers.
    // Returns true + fills `out` (16 kHz mono PCM16) exactly when an utterance just ended.
    bool poll(std::vector<int16_t>& out) {
        if (!hwi_) return false;
        const bool suspended = GetTickCount64() < suspend_until_ms_;
        bool got = false;
        for (int i = 0; i < kBufs; ++i) {
            if (!(hdr_[i].dwFlags & WHDR_DONE)) continue;
            const uint32_t n = hdr_[i].dwBytesRecorded / 2u;
            const int16_t* s = reinterpret_cast<const int16_t*>(bufmem_[i].data());
            if (suspended) {
                speaking_ = false; accum_.clear(); silence_chunks_ = 0;  // echo gate: drop + reset
            } else if (n > 0) {
                double sumsq = 0.0;
                for (uint32_t k = 0; k < n; ++k) { const double v = static_cast<double>(s[k]) / 32768.0; sumsq += v * v; }
                const float rms = static_cast<float>(std::sqrt(sumsq / n));
                if (warmup_ < kWarmupChunks) {                         // calibrate the noise floor first
                    noise_floor_ = (warmup_ == 0) ? rms : (0.5f * noise_floor_ + 0.5f * rms);
                    ++warmup_;
                } else {
                    const float speechT = std::max(noise_floor_ * kSpeechMult, kMinFloor);
                    const float endT = std::max(noise_floor_ * kEndMult, kMinFloor * 0.5f);
                    if (!speaking_) {
                        if (rms > speechT) { speaking_ = true; silence_chunks_ = 0; accum_.assign(s, s + n); }
                        else { noise_floor_ = 0.97f * noise_floor_ + 0.03f * rms; }   // track ambient drift
                    } else {
                        accum_.insert(accum_.end(), s, s + n);
                        if (rms < endT) ++silence_chunks_; else silence_chunks_ = 0;
                        const bool tooLong = accum_.size() >= kMaxUtterSamples;
                        if (silence_chunks_ >= kHangoverChunks || tooLong) {
                            if (accum_.size() >= kMinUtterSamples) { out.swap(accum_); got = true; }
                            speaking_ = false; accum_.clear(); silence_chunks_ = 0;
                        }
                    }
                }
            }
            hdr_[i].dwFlags &= ~WHDR_DONE;
            hdr_[i].dwBytesRecorded = 0;
            waveInAddBuffer(hwi_, &hdr_[i], sizeof(WAVEHDR));
            if (got) break;   // hand back one utterance per poll
        }
        return got;
    }

private:
    static constexpr int kBufs = 8;
    static constexpr uint32_t kBufSamples = 1600;       // 100 ms per buffer
    static constexpr uint32_t kWarmupChunks = 4;        // ~400 ms to learn the room
    static constexpr float kSpeechMult = 3.5f;          // speech starts at 3.5x the noise floor
    static constexpr float kEndMult = 2.0f;             // ...and ends below 2x (hysteresis)
    static constexpr float kMinFloor = 0.010f;          // absolute floor: ignore very quiet rooms' "speech"
    static constexpr uint32_t kHangoverChunks = 7;      // ~700 ms of silence ends an utterance
    static constexpr uint32_t kMinUtterSamples = kRate * 4u / 10u;   // 0.4 s minimum (reject clicks)
    static constexpr uint32_t kMaxUtterSamples = kRate * 12u;        // 12 s cap

    HWAVEIN hwi_ = nullptr;
    WAVEHDR hdr_[kBufs]{};
    std::vector<int16_t> bufmem_[kBufs];
    bool speaking_ = false;
    std::vector<int16_t> accum_;
    uint32_t silence_chunks_ = 0;
    uint32_t warmup_ = 0;
    float noise_floor_ = 0.0f;
    uint64_t suspend_until_ms_ = 0;
};

}  // namespace br::app
