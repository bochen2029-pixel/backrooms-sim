#include "core/lighting.h"

namespace br::core {

namespace {
uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}
float unit(uint64_t x) {
    return static_cast<float>(mix64(x) & 0xffffffULL) / static_cast<float>(0x1000000ULL);  // [0,1)
}
}  // namespace

float light_flicker(uint64_t world_seed, uint64_t light_id, uint64_t tick) noexcept {
    const uint64_t h = mix64(world_seed ^ (light_id * 0x9e3779b97f4a7c15ULL));
    if ((h & 7u) != 0u) return 1.0f;  // ~7/8 of lights are steady
    // Flickery light: value-noise interpolated over ~24 Hz steps, range [0.35,1].
    const uint64_t ti = tick / 5u;
    const float a = unit(h ^ ti);
    const float b = unit(h ^ (ti + 1u));
    const float f = static_cast<float>(tick % 5u) / 5.0f;
    const float n = a + (b - a) * f;
    return 0.35f + 0.65f * n;
}

}  // namespace br::core
