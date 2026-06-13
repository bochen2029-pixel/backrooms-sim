#include "gen/biome.h"

namespace br::gen {

namespace {

// Coarse cell = K x K chunks (K*32 m). Large enough that biomes read as regions,
// small enough that 100k chunks span enough independent cells for the +/-2 % gate.
constexpr int K = 3;

// Designed weights (percent), summing to 100. ClassicYellow dominates; the
// stranger biomes are progressively rarer.
const int kWeights[kBiomeCount] = {44, 22, 16, 12, 6};

uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Floor division (toward -inf) so coarse cells tile correctly across the origin.
int64_t floordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

}  // namespace

Biome biome_at(uint64_t world_seed, int32_t level, int64_t cx, int64_t cz) {
    const int64_t bx = floordiv(cx, K);
    const int64_t bz = floordiv(cz, K);
    uint64_t h = mix64(world_seed ^ (static_cast<uint64_t>(level) + 1u) * 0x9e3779b97f4a7c15ULL);
    h = mix64(h ^ static_cast<uint64_t>(bx) * 0xc2b2ae3d27d4eb4fULL);
    h = mix64(h ^ static_cast<uint64_t>(bz) * 0x165667b19e3779f9ULL);
    const int r = static_cast<int>(h % 100u);  // 0..99
    int acc = 0;
    for (int b = 0; b < kBiomeCount; ++b) {
        acc += kWeights[b];
        if (r < acc) return static_cast<Biome>(b);
    }
    return Biome::ClassicYellow;
}

int biome_weight_pct(Biome b) { return kWeights[static_cast<int>(b)]; }

BiomeParams biome_params(Biome b) {
    switch (b) {
        case Biome::ClassicYellow:
            return {0.25f, 0.00f, {1.00f, 1.00f, 1.00f}, 0.45f};
        case Biome::CubicleFarm:
            return {0.45f, 0.10f, {0.95f, 0.93f, 0.80f}, 0.55f};
        case Biome::PipeCorridors:
            return {0.08f, 0.04f, {0.70f, 0.72f, 0.74f}, 0.40f};
        case Biome::ParkingGarage:
            return {0.85f, 0.22f, {0.62f, 0.63f, 0.66f}, 0.50f};
        case Biome::Poolrooms:
            return {0.70f, 0.02f, {0.80f, 0.92f, 0.98f}, 0.60f};
        default:
            return {0.25f, 0.00f, {1.00f, 1.00f, 1.00f}, 0.45f};
    }
}

const char* biome_name(Biome b) {
    switch (b) {
        case Biome::ClassicYellow: return "classic_yellow";
        case Biome::CubicleFarm:   return "cubicle_farm";
        case Biome::PipeCorridors: return "pipe_corridors";
        case Biome::ParkingGarage: return "parking_garage";
        case Biome::Poolrooms:     return "poolrooms";
        default:                   return "unknown";
    }
}

}  // namespace br::gen
