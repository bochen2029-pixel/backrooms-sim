#pragma once
//
// audio/device.h — real-time playback device (M14), an opaque wrapper over a
// miniaudio ma_device.
//
// Opens the default playback endpoint (or miniaudio's hardware-free *null* backend
// for gates/CI) and calls `pull` from the device's audio thread to fill each
// buffer. No miniaudio types appear here (PIMPL), so only device.cpp ever sees
// <miniaudio.h> — the determinism-bearing modules never inherit it (INV-5 spirit).
//
#include <cstdint>
#include <memory>
#include <string>

namespace br::audio {

class AudioDevice {
public:
    // Fill `frames` interleaved `channels`-wide float frames into `out`. Invoked on
    // the device's real-time thread — must not block or allocate. `user` is opaque.
    using PullFn = void (*)(void* user, float* out, uint32_t frames, uint32_t channels);

    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Open + start playback at (sample_rate, channels) routing the callback to
    // `pull`. `use_null` forces the null backend (no hardware — the gated, CI-safe
    // path). Returns false (last_error set) if the device won't open.
    bool start(uint32_t sample_rate, uint32_t channels, PullFn pull, void* user, bool use_null);
    void stop();
    bool running() const noexcept;

    // Backend actually selected ("WASAPI", "Null", …); empty until started.
    const std::string& backend() const noexcept { return backend_; }
    const std::string& last_error() const noexcept { return last_error_; }

    struct Impl;  // opaque; defined in device.cpp (public so the file-local
                  // miniaudio data callback can reach it — mirrors renderer.h)

private:
    std::unique_ptr<Impl> impl_;
    std::string backend_;
    std::string last_error_;
};

}  // namespace br::audio
