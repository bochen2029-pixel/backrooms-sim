#pragma once
//
// contracts/chunk_gen_v1.h — gen -> stream/render boundary (v1.0).
//
// GenerateChunk is pure, total, deterministic (INV-2): output depends only on
// (WorldSeed, ChunkKey), with no neighbor queries. Cross-seam agreement is
// achieved by emitting vertices in WORLD coordinates, so adjacent chunks place
// boundary vertices at identical positions.
//
#include <cmath>
#include <cstdint>
#include <vector>

#include "contracts/geometry_v1.h"

namespace br::contracts {

constexpr float kChunkSize = 32.0f;  // meters per chunk edge

struct ChunkKey {
    int32_t level = 0;
    int64_t cx = 0;
    int64_t cz = 0;
};

inline bool operator==(const ChunkKey& a, const ChunkKey& b) {
    return a.level == b.level && a.cx == b.cx && a.cz == b.cz;
}
inline bool operator!=(const ChunkKey& a, const ChunkKey& b) { return !(a == b); }
inline bool operator<(const ChunkKey& a, const ChunkKey& b) {
    if (a.level != b.level) return a.level < b.level;
    if (a.cx != b.cx) return a.cx < b.cx;
    return a.cz < b.cz;
}

// The chunk containing world position (x, z) at a level.
inline ChunkKey chunk_key_at(int32_t level, float x, float z) {
    ChunkKey k;
    k.level = level;
    k.cx = static_cast<int64_t>(std::floor(x / kChunkSize));
    k.cz = static_cast<int64_t>(std::floor(z / kChunkSize));
    return k;
}

// Material ids (match render_d3d12::TexKind / texture-array slices).
constexpr float kMatWallpaper = 0.0f;
constexpr float kMatCarpet = 1.0f;
constexpr float kMatCeiling = 2.0f;
constexpr float kMatFluorescent = 3.0f;
constexpr float kMatBaseboard = 4.0f;

struct ChunkVertex {
    float pos[3];
    float nrm[3];
    float color[3];   // per-chunk tint (also drives the top-down debug view)
    float uv[2];
    float material;   // kMat* (selects texture-array slice; M5)
};

constexpr float kCellSize = kChunkSize / 8.0f;  // 4 m (matches gen layout)
constexpr float kCeilingHeight = 3.0f;

// Levels stack vertically (M7 verticality): level L's floor sits at this world Y,
// its ceiling at +kCeilingHeight, with a 1 m slab below before the next level.
// Level 0 maps to Y=0, so level-0 geometry is unchanged.
constexpr float kLevelHeight = 4.0f;
inline float level_base_y(int32_t level) { return static_cast<float>(level) * kLevelHeight; }

// Fluorescent ceiling cells form a regular grid (the backrooms light grid):
// every other global cell in both axes. gen tiles + renderer lights agree here.
inline bool is_fluorescent_cell(int64_t gi, int64_t gj) {
    return ((gi & 1) == 0) && ((gj & 1) == 0);
}

// World-space position of the fluorescent light at fluorescent cell (gi, gj).
inline void fluorescent_light_pos(int64_t gi, int64_t gj, float out[3]) {
    out[0] = (static_cast<float>(gi) + 0.5f) * kCellSize;
    out[1] = kCeilingHeight;
    out[2] = (static_cast<float>(gj) + 0.5f) * kCellSize;
}

struct ChunkData {
    ChunkKey key;
    std::vector<ChunkVertex> vertices;     // world-space triangle list (render)
    std::vector<BoxInstance> collision;    // world-space solid wall AABBs (sim)
    uint64_t content_hash = 0;
};

// Implemented by `gen`. Pure/total/deterministic.
ChunkData GenerateChunk(uint64_t world_seed, ChunkKey key);
uint64_t ChunkContentHash(const ChunkData& c);

// Geometry validator: every collision box is a valid wall (thin in exactly one
// axis) or a small square pillar (thin in both, <= 1 m; M7), none degenerate/
// floating/fat/overlapping (M4 gate, extended for pillars in ADR-032).
bool ValidateChunkGeometry(const ChunkData& c);

}  // namespace br::contracts
