#pragma once
//
// gen/biome.h — the biome field (M7).
//
// A biome is assigned to each chunk by a **low-frequency**, pure, deterministic
// function of (WorldSeed, level, chunk coords): a coarse lattice of K x K-chunk
// cells, each drawn from a weighted distribution, so biomes form contiguous
// regions with controllable proportions (the M7 distribution gate). The biome
// selects generation parameters (openness, pillars, materials) but never the
// edge-doorway protocol — so connectivity across biome seams is preserved
// exactly as in M4 (INV-3).
//
#include <cstdint>

namespace br::gen {

enum class Biome : uint8_t {
    ClassicYellow = 0,  // the canonical yellow maze
    CubicleFarm,        // open office grid: low partitions + pillars
    PipeCorridors,      // tight winding corridors
    ParkingGarage,      // very open, regular pillar grid
    Poolrooms,          // open tiled rooms
    Count
};
constexpr int kBiomeCount = static_cast<int>(Biome::Count);

// Per-biome generation parameters (consumed by GenerateChunk in M7).
struct BiomeParams {
    float carve_ratio;      // P(remove an interior wall) — higher = more open
    float pillar_density;   // P(an open cell gets a decorative pillar) [0,1]
    float floor_tint[3];    // multiplies the per-chunk base floor colour
    float wall_darken;      // wall colour = floor colour * wall_darken
};

// Low-frequency biome assignment. Pure/total/deterministic (INV-2).
Biome biome_at(uint64_t world_seed, int32_t level, int64_t cx, int64_t cz);

// Designed proportion (percent, summing to 100) — the target the distribution
// gate checks the realized field against (+/- 2 % over 100k chunks).
int biome_weight_pct(Biome b);

BiomeParams biome_params(Biome b);

const char* biome_name(Biome b);

}  // namespace br::gen
