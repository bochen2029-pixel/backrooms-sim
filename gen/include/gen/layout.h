#pragma once
//
// gen/layout.h — the Level-0 chunk layout solver (M4).
//
// A chunk is a G×G grid of cells. `generate_layout` builds a provably-connected
// maze: a spanning tree over the cells (recursive backtracker) plus extra carved
// openings for loops/rooms, then a doorway on each of the 4 chunk edges whose
// position comes from a SHARED-edge hash so adjacent chunks agree without
// communication (INV-2/INV-3). `validate_connectivity` flood-fills the cell grid.
//
#include <cstdint>
#include <vector>

#include "contracts/chunk_gen_v1.h"

namespace br::gen {

constexpr int kCellsPerChunk = 8;                                    // G
constexpr float kCellSize = contracts::kChunkSize / kCellsPerChunk;  // 4 m

// Wall presence on the cell-grid lines. A wall between two cells blocks passage.
struct ChunkLayout {
    // Vertical walls: vwall[xi][j] sits on x-line xi (0..G) for z-cell j (0..G-1).
    bool vwall[kCellsPerChunk + 1][kCellsPerChunk];
    // Horizontal walls: hwall[i][zj] sits on z-line zj (0..G) for x-cell i (0..G-1).
    bool hwall[kCellsPerChunk][kCellsPerChunk + 1];
    // Doorway cell index (0..G-1) on each chunk edge (a gap in the perimeter wall).
    int door_left, door_right, door_bottom, door_top;
};

// Pure/total/deterministic. Doorways agree across shared edges (see edge hashes).
// Uses the chunk's biome to set interior openness (carve ratio).
ChunkLayout generate_layout(uint64_t world_seed, contracts::ChunkKey key);

// As above but with an explicit interior-wall carve ratio (the biome openness
// knob). Connectivity holds for any ratio in [0,1] — carving only removes walls
// atop the spanning tree, never disconnects. Exposed for per-biome validation.
ChunkLayout generate_layout_carve(uint64_t world_seed, contracts::ChunkKey key,
                                  float carve_ratio);

// Flood-fill: returns true iff every cell is reachable from cell (0,0). A correct
// layout is connected by construction; the validator guards against regressions.
bool validate_connectivity(const ChunkLayout& layout);

// Master per-chunk RNG seed (shared by layout + chunk geometry tint).
uint64_t chunk_seed(uint64_t world_seed, contracts::ChunkKey key);

// A descending stairwell set piece (M7 verticality): solid step boxes that walk
// the wanderer down from level `top_level` to `top_level - 1`, starting at world
// (x0, z0) and running +X over `kStairWidth` in Z. Collision-only (the sim
// descends it under gravity; rendering integrates later). Appends to `out`.
constexpr float kStairWidth = 8.0f;
void build_stairwell(float x0, float z0, int32_t top_level,
                     std::vector<contracts::BoxInstance>& out);

}  // namespace br::gen
