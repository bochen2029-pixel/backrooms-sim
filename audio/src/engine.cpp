#include "audio/engine.h"

#include <chrono>
#include <cstring>
#include <vector>

#include "audio/device.h"

namespace br::audio {

AudioEngine::AudioEngine(uint64_t seed, uint32_t sample_rate, uint32_t block_frames)
    : seed_(seed), sr_(sample_rate), block_(block_frames) {}

AudioEngine::~AudioEngine() { stop(); }

void AudioEngine::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

bool AudioEngine::start_device(bool use_null) {
    if (running_.exchange(true)) return false;
    // ~16 blocks (~170 ms at 48 kHz / 512) of headroom — the same the M6 model used.
    ring_.reset(static_cast<size_t>(block_) * 16u, contracts::kAudioChannels);
    device_ = std::make_unique<AudioDevice>();

    // Start the producer first; it primes the ring from a fresh synth. Then wait
    // (bounded) until the ring is full so the device's first callback has data —
    // avoids a spurious startup underrun.
    thread_ = std::thread([this] { run_device(); });
    for (int i = 0; i < 1000 && ring_.writable_frames() > block_; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    if (!device_->start(sr_, contracts::kAudioChannels, &AudioEngine::pull_trampoline, this,
                        use_null)) {
        running_.store(false, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
        device_.reset();
        return false;
    }
    device_open_.store(true, std::memory_order_relaxed);
    return true;
}

void AudioEngine::stop() {
    if (!running_.exchange(false)) return;
    if (device_) device_->stop();  // halt callbacks before joining the producer
    if (thread_.joinable()) thread_.join();
    device_.reset();
    device_open_.store(false, std::memory_order_relaxed);
}

void AudioEngine::post(const contracts::AudioListener& listener, float reverb_seconds,
                       uint32_t new_footsteps) {
    speed_.store(listener.speed, std::memory_order_relaxed);
    reverb_.store(reverb_seconds, std::memory_order_relaxed);
    if (new_footsteps) footstep_credits_.fetch_add(new_footsteps, std::memory_order_relaxed);
}

const char* AudioEngine::backend() const { return device_ ? device_->backend().c_str() : ""; }

// Render one interleaved-stereo block: fold in the latest lock-free params from the
// sim thread, synthesize, then apply the playback mix (SFX gain on footsteps, master
// on the block). Shared by both producers. master/sfx default to 1.0, so the M6
// headless soak (which never sets them) behaves exactly as before.
void AudioEngine::render_block(Synth& synth, float* buf) {
    synth.set_reverb_seconds(reverb_.load(std::memory_order_relaxed));
    synth.set_draft(draft_.load(std::memory_order_relaxed));  // M30 telegraph (0 by default -> inert)
    const float sfx = sfx_.load(std::memory_order_relaxed);
    uint32_t credits = footstep_credits_.exchange(0, std::memory_order_relaxed);
    while (credits-- > 0) synth.trigger_footstep(0.8f * sfx);
    contracts::AudioListener l{};
    l.speed = speed_.load(std::memory_order_relaxed);
    synth.render(l, buf, block_);
    const float m = master_.load(std::memory_order_relaxed);
    if (m != 1.0f) {
        const size_t n = static_cast<size_t>(block_) * contracts::kAudioChannels;
        for (size_t i = 0; i < n; ++i) buf[i] *= m;
    }
}

void AudioEngine::pull_trampoline(void* user, float* out, uint32_t frames, uint32_t channels) {
    static_cast<AudioEngine*>(user)->pull(out, frames, channels);
}

// Consumer — runs on the device's real-time thread. Drains the ring; if it ran dry
// (the mixer fell behind) fill the gap with silence and count one underrun.
void AudioEngine::pull(float* out, uint32_t frames, uint32_t channels) {
    const size_t got = ring_.pop(out, frames);
    if (got < frames) {
        std::memset(out + got * channels, 0,
                    static_cast<size_t>(frames - got) * channels * sizeof(float));
        underruns_.fetch_add(1, std::memory_order_relaxed);
    }
}

// M14 producer: keep the ring full ahead of the device callback. Naps half a block
// when full so it stays comfortably ahead without busy-spinning.
void AudioEngine::run_device() {
    using clock = std::chrono::steady_clock;
    Synth synth(seed_, sr_);
    std::vector<float> buf(static_cast<size_t>(block_) * contracts::kAudioChannels, 0.0f);
    const auto nap = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(static_cast<double>(block_) / (2.0 * static_cast<double>(sr_))));
    while (running_.load(std::memory_order_relaxed)) {
        if (ring_.writable_frames() >= block_) {
            render_block(synth, buf.data());
            ring_.push(buf.data(), block_);
            blocks_.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::this_thread::sleep_for(nap);
        }
    }
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
            render_block(synth, buf.data());
            produced += block_;
            blocks_.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::this_thread::sleep_for(nap);  // comfortably ahead; nap one block
        }
    }
}

}  // namespace br::audio
