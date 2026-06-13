#include "audio/room_probe.h"

#include <cmath>

namespace br::audio {

namespace {

// Nearest forward hit of a horizontal ray (origin ox,oz; unit dir dx,dz) against
// an AABB's XZ footprint, using the slab method. Returns a large sentinel if the
// ray misses or only hits behind the origin.
constexpr float kFar = 60.0f;

float ray_box_xz(float ox, float oz, float dx, float dz, const contracts::BoxInstance& b) {
    float tenter = 0.0f, texit = kFar;
    // X slab.
    if (std::fabs(dx) > 1e-6f) {
        float t1 = (b.mn[0] - ox) / dx, t2 = (b.mx[0] - ox) / dx;
        if (t1 > t2) { const float tmp = t1; t1 = t2; t2 = tmp; }
        if (t1 > tenter) tenter = t1;
        if (t2 < texit) texit = t2;
    } else if (ox < b.mn[0] || ox > b.mx[0]) {
        return kFar;  // parallel and outside the slab
    }
    // Z slab.
    if (std::fabs(dz) > 1e-6f) {
        float t1 = (b.mn[2] - oz) / dz, t2 = (b.mx[2] - oz) / dz;
        if (t1 > t2) { const float tmp = t1; t1 = t2; t2 = tmp; }
        if (t1 > tenter) tenter = t1;
        if (t2 < texit) texit = t2;
    } else if (oz < b.mn[2] || oz > b.mx[2]) {
        return kFar;
    }
    if (tenter > texit || texit <= 0.0f) return kFar;
    return (tenter > 0.0f) ? tenter : kFar;  // inside-the-box hits don't count
}

}  // namespace

float probe_mean_free_path(const contracts::AudioListener& listener,
                           const std::vector<contracts::BoxInstance>& walls) {
    constexpr int kRays = 16;
    const float ox = listener.pos[0], oz = listener.pos[2];
    float sum = 0.0f;
    for (int i = 0; i < kRays; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(kRays);
        const float dx = std::cos(a), dz = std::sin(a);
        float nearest = kFar;
        for (const auto& b : walls) {
            const float t = ray_box_xz(ox, oz, dx, dz, b);
            if (t < nearest) nearest = t;
        }
        sum += nearest;
    }
    return sum / static_cast<float>(kRays);
}

float probe_reverb_seconds(const contracts::AudioListener& listener,
                           const std::vector<contracts::BoxInstance>& walls) {
    const float mfp = probe_mean_free_path(listener, walls);
    // Map mean free path -> reverb time: tight corridor (~2 m) ~0.27 s, open
    // hall (~30 m) ~1.6 s. Clamped so the comb feedback stays stable.
    float rt = 0.15f + mfp * 0.05f;
    if (rt < 0.20f) rt = 0.20f;
    if (rt > 1.80f) rt = 1.80f;
    return rt;
}

}  // namespace br::audio
