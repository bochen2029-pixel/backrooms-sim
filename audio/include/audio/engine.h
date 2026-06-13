#pragma once
//
// audio/engine.h — headless real-time mixer thread (M6 soak).
//
// A background thread renders the synth in fixed blocks, paced by a wall-clock
// deadline schedule (the stand-in for a device callback — no speakers needed in
// the gate). The sim thread hands it parameters through lock-free atomics, so
// posting never blocks the tick loop. Underruns = blocks that missed their
// real-time deadline (the mixer fell behind), which must stay at zero.
//
// Wall-clock lives here on purpose: this is the real-time path, not the
// deterministic offline render. It touches no sim state and no goldens.
//
#include <atomic>
#include <cstdint>
#include <thread>

#include "audio/synth.h"
#include "contracts/audio_events_v1.h"

namespace br::audio {

class AudioEngine {
public:
    AudioEngine(uint64_t seed, uint32_t sample_rate, uint32_t block_frames = 512);
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    void start();
    void stop();

    // Lock-free, non-blocking update from the sim thread.
    void post(const contracts::AudioListener& listener, float reverb_seconds, uint32_t new_footsteps);

    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    uint64_t blocks_rendered() const { return blocks_.load(std::memory_order_relaxed); }

private:
    void run();

    uint64_t seed_;
    uint32_t sr_;
    uint32_t block_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::atomic<float> reverb_{0.5f};
    std::atomic<float> speed_{0.0f};
    std::atomic<uint32_t> footstep_credits_{0};

    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> blocks_{0};
};

}  // namespace br::audio
