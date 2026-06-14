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
#include <memory>
#include <thread>

#include "audio/ring.h"
#include "audio/synth.h"
#include "contracts/audio_events_v1.h"

namespace br::audio {

class AudioDevice;  // audio/device.h — opaque (keeps <miniaudio.h> out of here)

class AudioEngine {
public:
    AudioEngine(uint64_t seed, uint32_t sample_rate, uint32_t block_frames = 512);
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // M6 headless path: a wall-clock-paced producer renders into a null sink and
    // counts underruns against a virtual read cursor. No device, no speakers.
    void start();

    // M14 real-time path: open a playback device (or the null backend when
    // `use_null`) and run the mixer as the device's ring producer — the callback
    // drains the ring on its own thread. Returns false if the device won't open.
    bool start_device(bool use_null);

    void stop();  // stops whichever path is running; safe to call repeatedly

    // Lock-free, non-blocking update from the sim thread.
    void post(const contracts::AudioListener& listener, float reverb_seconds, uint32_t new_footsteps);

    // Playback mix controls (presentation only; never touch the offline render).
    void set_master_volume(float v) { master_.store(v, std::memory_order_relaxed); }
    void set_sfx_volume(float v) { sfx_.store(v, std::memory_order_relaxed); }

    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    uint64_t blocks_rendered() const { return blocks_.load(std::memory_order_relaxed); }
    bool device_open() const { return device_open_.load(std::memory_order_relaxed); }
    const char* backend() const;

private:
    void run();         // M6 virtual-cursor producer
    void run_device();  // M14 ring-filling producer
    void render_block(Synth& synth, float* buf);  // shared: pull params + synth a block
    void pull(float* out, uint32_t frames, uint32_t channels);  // consumer (device thread)
    static void pull_trampoline(void* user, float* out, uint32_t frames, uint32_t channels);

    uint64_t seed_;
    uint32_t sr_;
    uint32_t block_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    FloatRing ring_;
    std::unique_ptr<AudioDevice> device_;

    std::atomic<float> reverb_{0.5f};
    std::atomic<float> speed_{0.0f};
    std::atomic<uint32_t> footstep_credits_{0};
    std::atomic<float> master_{1.0f};
    std::atomic<float> sfx_{1.0f};

    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> blocks_{0};
    std::atomic<bool> device_open_{false};
};

}  // namespace br::audio
