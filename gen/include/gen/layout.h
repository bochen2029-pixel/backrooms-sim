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

// M27 (Phase IV): procedural placement of an UP-stair from level L to L+1. A pure
// shared-seam function read identically from BOTH floors -- L builds the up-stairwell +
// cuts its ceiling hole; L+1 reads the SAME stair_at(seed, L, cx, cz) to cut its floor
// hole + place the landing (the Z-analogue of the door hash; INV-2, no neighbour query).
// HYBRID coverage: a density scatter (~1 per kStairDensityN chunks) PLUS a guaranteed
// per-superblock backstop, so every kStairSuperblock x kStairSuperblock block holds >=1
// up-stair -> the stack is vertically connected within a bounded XZ distance (INV-3 in Z).
// Pure/total: only hash evals (<= one block scan); never generates a chunk.
constexpr int kStairSuperblock = 4;   // 4x4 chunks (~128 m) -- the K=4 hard backstop
constexpr int kStairDensityN  = 13;   // ~1 density up-stair per N chunks (backstop fills empties)
struct StairSpec {
    bool present = false;          // is there an up-stair (level -> level+1) in this chunk?
    int cell_i = 0, cell_j = 0;    // its cell in the G x G grid (0..G-1)
};
StairSpec stair_at(uint64_t world_seed, int32_t level, int64_t cx, int64_t cz);

}  // namespace br::gen
