#pragma once
//
// audio/ring.h — single-producer / single-consumer lock-free float ring (M14).
//
// The M6 mixer modelled "a device callback + ring" against a virtual clock; M14
// makes the ring real. The mixer thread pushes rendered interleaved-stereo blocks
// (producer); miniaudio's device callback pops them on its own real-time thread
// (consumer). Sized once at reset(); never allocates afterwards. Wait-free on both
// ends — only the two cursors are shared, via release/acquire.
//
#include <atomic>
#include <cstdint>
#include <vector>

namespace br::audio {

class FloatRing {
public:
    // Capacity is in *frames* (one frame = `channels` interleaved floats). One slot
    // is kept empty to disambiguate full from empty, so usable frames == capacity.
    void reset(size_t capacity_frames, uint32_t channels) {
        ch_ = channels ? channels : 1u;
        cap_ = capacity_frames + 1u;
        buf_.assign(cap_ * ch_, 0.0f);
        w_.store(0, std::memory_order_relaxed);
        r_.store(0, std::memory_order_relaxed);
    }

    uint32_t channels() const noexcept { return ch_; }
    size_t capacity_frames() const noexcept { return cap_ ? cap_ - 1u : 0u; }

    size_t readable_frames() const noexcept {
        const size_t w = w_.load(std::memory_order_acquire);
        const size_t r = r_.load(std::memory_order_relaxed);
        return (w + cap_ - r) % cap_;
    }
    size_t writable_frames() const noexcept { return capacity_frames() - readable_frames(); }

    // Producer: copy up to `frames` frames from `src`; returns frames written.
    size_t push(const float* src, size_t frames) noexcept {
        const size_t w = w_.load(std::memory_order_relaxed);
        const size_t r = r_.load(std::memory_order_acquire);
        const size_t space = (r + cap_ - w - 1u) % cap_;
        const size_t n = frames < space ? frames : space;
        for (size_t i = 0; i < n; ++i) {
            const size_t slot = (w + i) % cap_;
            for (uint32_t c = 0; c < ch_; ++c) buf_[slot * ch_ + c] = src[i * ch_ + c];
        }
        w_.store((w + n) % cap_, std::memory_order_release);
        return n;
    }

    // Consumer: copy up to `frames` frames into `dst`; returns frames read.
    size_t pop(float* dst, size_t frames) noexcept {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t w = w_.load(std::memory_order_acquire);
        const size_t avail = (w + cap_ - r) % cap_;
        const size_t n = frames < avail ? frames : avail;
        for (size_t i = 0; i < n; ++i) {
            const size_t slot = (r + i) % cap_;
            for (uint32_t c = 0; c < ch_; ++c) dst[i * ch_ + c] = buf_[slot * ch_ + c];
        }
        r_.store((r + n) % cap_, std::memory_order_release);
        return n;
    }

private:
    std::vector<float> buf_;
    size_t cap_ = 0;               // slots == usable frames + 1
    uint32_t ch_ = 2;
    std::atomic<size_t> w_{0};     // write cursor (producer advances)
    std::atomic<size_t> r_{0};     // read cursor (consumer advances)
};

}  // namespace br::audio
