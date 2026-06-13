#include "audio/engine.h"

#include <chrono>
#include <vector>

namespace br::audio {

AudioEngine::AudioEngine(uint64_t seed, uint32_t sample_rate, uint32_t block_frames)
    : seed_(seed), sr_(sample_rate), block_(block_frames) {}

AudioEngine::~AudioEngine() { stop(); }

void AudioEngine::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void AudioEngine::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void AudioEngine::post(const contracts::AudioListener& listener, float reverb_seconds,
                       uint32_t new_footsteps) {
    speed_.store(listener.speed, std::memory_order_relaxed);
    reverb_.store(reverb_seconds, std::memory_order_relaxed);
    if (new_footsteps) footstep_credits_.fetch_add(new_footsteps, std::memory_order_relaxed);
}

void AudioEngine::run() {
    using clock = std::chrono::steady_clock;
    Synth synth(seed_, sr_);
    std::vector<float> buf(static_cast<size_t>(block_) * 2u, 0.0f);
    const double sr_d = static_cast<double>(sr_);

    // Prebuffered producer model: render ahead of a virtual real-time read cursor
    // and keep ~170 ms of headroom, exactly as a device callback + ring would. An
    // underrun is the buffer genuinely running dry (the mixer fell > headroom
    // behind real time) — not mere OS sleep jitter, which the headroom absorbs.
    const uint64_t headroom = static_cast<uint64_t>(block_) * 16u;
    const auto nap = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(static_cast<double>(block_) / sr_d));

    const auto start = clock::now();
    uint64_t produced = 0;  // frames synthesized (the ring write cursor)
    while (running_.load(std::memory_order_relaxed)) {
        const double elapsed = std::chrono::duration<double>(clock::now() - start).count();
        const uint64_t required = static_cast<uint64_t>(elapsed * sr_d);  // device read cursor
        if (produced < required) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            produced = required;  // resync, as a real device would on a dropout
        }
        if (produced < required + headroom) {
            // Pull the latest parameters posted by the sim thread (lock-free).
            synth.set_reverb_seconds(reverb_.load(std::memory_order_relaxed));
            uint32_t credits = footstep_credits_.exchange(0, std::memory_order_relaxed);
            while (credits-- > 0) synth.trigger_footstep(0.8f);
            contracts::AudioListener l{};
            l.speed = speed_.load(std::memory_order_relaxed);
            synth.render(l, buf.data(), block_);  // output -> null sink
            produced += block_;
            blocks_.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::this_thread::sleep_for(nap);  // comfortably ahead; nap one block
        }
    }
}

}  // namespace br::audio
