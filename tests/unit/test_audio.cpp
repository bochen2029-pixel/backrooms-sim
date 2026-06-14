// test_audio — M6 procedural audio gates: synth determinism, WAV round-trip,
// room-probe sizing, the 60 Hz hum is actually present, and footstep timing is a
// pure function of the odometer.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "audio/ring.h"
#include "audio/room_probe.h"
#include "audio/synth.h"
#include "audio/wav.h"
#include "contracts/audio_events_v1.h"
#include "contracts/geometry_v1.h"
#include "core/world.h"

using namespace br;

namespace {

contracts::AudioListener still_listener() {
    contracts::AudioListener l{};
    l.pos[0] = 0.0f; l.pos[1] = 1.62f; l.pos[2] = 0.0f;
    l.yaw = 0.0f; l.speed = 0.0f;
    return l;
}

// Goertzel power of frequency f over a mono signal.
double goertzel_power(const std::vector<float>& x, double f, uint32_t sr) {
    const double w = 2.0 * 3.141592653589793 * f / static_cast<double>(sr);
    const double cw = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (float v : x) { s0 = static_cast<double>(v) + cw * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - cw * s1 * s2;
}

std::vector<contracts::BoxInstance> square_room(float r) {
    const float h = 3.0f, t = 0.1f;
    return {
        {{-r, 0.0f, r - t}, {r, h, r + t}},   // +Z wall
        {{-r, 0.0f, -r - t}, {r, h, -r + t}},  // -Z wall
        {{r - t, 0.0f, -r}, {r + t, h, r}},    // +X wall
        {{-r - t, 0.0f, -r}, {-r + t, h, r}},  // -X wall
    };
}

}  // namespace

TEST_CASE("synth output is deterministic for a fixed seed and call sequence", "[m6][audio]") {
    const uint32_t sr = contracts::kAudioSampleRate;
    const uint32_t frames = 4096;
    audio::Synth a(1234u, sr), b(1234u, sr);
    a.set_reverb_seconds(0.6f); b.set_reverb_seconds(0.6f);
    a.trigger_footstep(0.8f); b.trigger_footstep(0.8f);
    std::vector<float> ba(2 * frames), bb(2 * frames);
    a.render(still_listener(), ba.data(), frames);
    b.render(still_listener(), bb.data(), frames);
    REQUIRE(ba == bb);
}

TEST_CASE("different seeds diverge the noise bed", "[m6][audio]") {
    const uint32_t sr = contracts::kAudioSampleRate, frames = 2048;
    audio::Synth a(1u, sr), b(2u, sr);
    std::vector<float> ba(2 * frames), bb(2 * frames);
    a.render(still_listener(), ba.data(), frames);
    b.render(still_listener(), bb.data(), frames);
    REQUIRE(ba != bb);
}

TEST_CASE("WAV PCM16 round-trips exactly", "[m6][audio]") {
    std::vector<int16_t> in;
    for (int i = 0; i < 2000; ++i) in.push_back(static_cast<int16_t>((i * 37 % 65535) - 32768));
    const std::string path = "test_audio_roundtrip.wav";
    std::string err;
    REQUIRE(audio::write_wav(path, in, contracts::kAudioSampleRate, 2, err));
    std::vector<int16_t> out; uint32_t sr = 0; uint16_t ch = 0;
    REQUIRE(audio::read_wav(path, out, sr, ch, err));
    REQUIRE(sr == contracts::kAudioSampleRate);
    REQUIRE(ch == 2);
    REQUIRE(out == in);
}

TEST_CASE("the fluorescent hum puts energy at 60 Hz", "[m6][audio]") {
    const uint32_t sr = contracts::kAudioSampleRate, frames = sr;  // 1 second
    audio::Synth s(7u, sr);
    std::vector<float> stereo(2 * frames);
    s.render(still_listener(), stereo.data(), frames);
    std::vector<float> mono(frames);
    for (uint32_t i = 0; i < frames; ++i) mono[i] = 0.5f * (stereo[2 * i] + stereo[2 * i + 1]);
    const double p60 = goertzel_power(mono, 60.0, sr);
    const double p1000 = goertzel_power(mono, 1000.0, sr);
    REQUIRE(p60 > p1000 * 50.0);  // the mains tone dominates a quiet upper band
}

TEST_CASE("room probe: a larger room reverberates longer", "[m6][audio]") {
    const auto small = square_room(3.0f);
    const auto big = square_room(25.0f);
    const auto l = still_listener();
    REQUIRE(audio::probe_mean_free_path(l, small) < audio::probe_mean_free_path(l, big));
    REQUIRE(audio::probe_reverb_seconds(l, small) < audio::probe_reverb_seconds(l, big));
    // Tight space stays near the floor; the open hall rings longer.
    REQUIRE(audio::probe_reverb_seconds(l, small) < 0.5f);
    REQUIRE(audio::probe_reverb_seconds(l, big) > 1.0f);
}

TEST_CASE("footstep count is a pure floor of the odometer", "[m6][audio]") {
    core::WorldState s(42u);
    s.odometer = 0.0f;     REQUIRE(core::footstep_count(s) == 0u);
    s.odometer = 1.59f;    REQUIRE(core::footstep_count(s) == 0u);
    s.odometer = 1.6f;     REQUIRE(core::footstep_count(s) == 1u);
    s.odometer = 3.21f;    REQUIRE(core::footstep_count(s) == 2u);
    // Monotone non-decreasing as the wanderer advances.
    uint64_t prev = 0;
    for (float d = 0.0f; d < 100.0f; d += 0.13f) {
        s.odometer = d;
        const uint64_t c = core::footstep_count(s);
        REQUIRE(c >= prev);
        prev = c;
    }
}

// --- M14: the SPSC float ring (the real mixer -> device hand-off) -------------

TEST_CASE("float ring: empty, fill, wrap, and data integrity", "[m14][audio]") {
    audio::FloatRing ring;
    ring.reset(4, 2);  // 4 frames usable, stereo
    REQUIRE(ring.capacity_frames() == 4u);
    REQUIRE(ring.readable_frames() == 0u);
    REQUIRE(ring.writable_frames() == 4u);

    float out[8] = {0};
    REQUIRE(ring.pop(out, 4) == 0u);  // nothing to read yet

    // Frame i carries {i, i+100} so we can verify exact round-trip + channel order.
    auto frame = [](size_t i, float* f) { f[0] = static_cast<float>(i); f[1] = static_cast<float>(i) + 100.0f; };
    float in[8];
    for (size_t i = 0; i < 4; ++i) frame(i, in + i * 2);
    REQUIRE(ring.push(in, 4) == 4u);
    REQUIRE(ring.writable_frames() == 0u);
    REQUIRE(ring.push(in, 1) == 0u);  // full: rejects further frames

    // Drain 2, push 2 more -> forces a wrap across the capacity boundary.
    REQUIRE(ring.pop(out, 2) == 2u);
    REQUIRE(out[0] == 0.0f);  REQUIRE(out[1] == 100.0f);
    REQUIRE(out[2] == 1.0f);  REQUIRE(out[3] == 101.0f);
    float in2[4]; frame(4, in2); frame(5, in2 + 2);
    REQUIRE(ring.push(in2, 2) == 2u);

    // Remaining order must be frames 2,3,4,5 (FIFO across the wrap).
    float all[8];
    REQUIRE(ring.pop(all, 4) == 4u);
    for (size_t i = 0; i < 4; ++i) {
        REQUIRE(all[i * 2] == static_cast<float>(i + 2));
        REQUIRE(all[i * 2 + 1] == static_cast<float>(i + 2) + 100.0f);
    }
    REQUIRE(ring.readable_frames() == 0u);
}

TEST_CASE("float ring: single-producer single-consumer loses no frames", "[m14][audio]") {
    audio::FloatRing ring;
    ring.reset(256, 2);
    constexpr size_t kTotal = 200000;

    std::thread producer([&] {
        size_t sent = 0;
        float f[2];
        while (sent < kTotal) {
            f[0] = static_cast<float>(sent & 0xFFFFu);
            f[1] = -f[0];
            if (ring.push(f, 1) == 1u) ++sent;  // spin until space (consumer drains)
        }
    });

    size_t got = 0;
    bool ok = true;
    float f[2];
    while (got < kTotal) {
        if (ring.pop(f, 1) == 1u) {
            const float expect = static_cast<float>(got & 0xFFFFu);
            if (f[0] != expect || f[1] != -expect) ok = false;
            ++got;
        }
    }
    producer.join();
    REQUIRE(ok);            // every frame arrived in order, uncorrupted
    REQUIRE(got == kTotal);
}
