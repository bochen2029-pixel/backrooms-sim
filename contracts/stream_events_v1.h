#pragma once
//
// contracts/stream_events_v1.h — stream -> renderers boundary (v1.0).
//
// The stream ring owns resident ChunkData; renderers consume a read-only
// snapshot of it each frame. Memory is bounded by the ring (INV-4).
//
#include <cstdint>

#include "contracts/chunk_gen_v1.h"

namespace br::contracts {

// A resident chunk as the renderer sees it (read-only, valid for the frame).
struct ResidentChunk {
    ChunkKey key;
    const ChunkVertex* vertices;
    uint32_t vertex_count;
};

enum class ResidencyEvent { Generated, Evicted };

}  // namespace br::contracts
