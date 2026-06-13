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

struct ChunkVertex {
    float pos[3];
    float nrm[3];
    float color[3];
};

struct ChunkData {
    ChunkKey key;
    std::vector<ChunkVertex> vertices;     // world-space triangle list (render)
    std::vector<BoxInstance> collision;    // world-space solid wall AABBs (sim)
    uint64_t content_hash = 0;
};

// Implemented by `gen`. Pure/total/deterministic.
ChunkData GenerateChunk(uint64_t world_seed, ChunkKey key);
uint64_t ChunkContentHash(const ChunkData& c);

// Geometry validator: no degenerate/floating/fat/overlapping walls (M4 gate).
bool ValidateChunkGeometry(const ChunkData& c);

}  // namespace br::contracts
