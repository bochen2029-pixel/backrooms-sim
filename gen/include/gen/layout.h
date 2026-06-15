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

// M27: the vertical-connectivity validator (the Z-analogue of validate_connectivity).
// Over a finite slab of the stack -- levels [lvl_lo, lvl_hi] x chunks in [-radius, radius]^2
// -- treat each chunk as a node: horizontal neighbours are ALWAYS linked (every shared edge
// carries a doorway), and (L,cx,cz)<->(L+1,cx,cz) is linked iff stair_at fires there. Returns
// true iff the whole slab is ONE connected component -- i.e. no floor is vertically sealed
// (every floor reaches every other within a bounded distance). Pure/total (hash evals only);
// radius >= kStairSuperblock is enough for the per-superblock backstop to guarantee success.
bool validate_vertical_connectivity(uint64_t world_seed, int32_t lvl_lo, int32_t lvl_hi, int radius);

// M30 (Phase IV): open shafts -- a rare vertical VOID that drops you DOWN several floors (the
// "despair gradient": you sink faster than you climb). Far rarer than stairs (~1 per
// kShaftDensityN chunks -- the design's ~1.3 km cadence). Each shaft is a fixed column (cx,cz)
// with a hashed cell, top level, and depth (kShaftDepthMin..Max floors). Pure/total per column;
// any level decides LOCALLY whether the void passes through it (shaft_passes) -- no neighbour
// query, the Z-analogue of the stair seam. (Geometry/soft-catch fall/fog render come next in M30.)
constexpr int kShaftDensityN  = 1500;   // ~1 shaft per N chunks (very rare; ~1.3 km area cadence)
constexpr int kShaftDepthMin  = 5;      // floors dropped (locked design: deep 5..10)
constexpr int kShaftDepthMax  = 10;
constexpr int32_t kShaftLevelBand = 30; // top level hashed into [-band, band] (near-origin; M31 widens)
struct ShaftSpec {
    bool present = false;
    int cell_i = 0, cell_j = 0;         // its cell in the G x G grid (0..G-1)
    int32_t top_level = 0;              // the floor you can fall in from
    int32_t depth = 0;                  // floors it drops (kShaftDepthMin..kShaftDepthMax)
};
ShaftSpec shaft_at(uint64_t world_seed, int64_t cx, int64_t cz);

// Does the shaft's void occupy `level`'s space at its column? The void spans the inclusive
// range [top_level - depth, top_level]: you fall in at top_level and land on (top_level - depth).
inline bool shaft_passes(const ShaftSpec& s, int32_t level) {
    return s.present && level <= s.top_level && level >= s.top_level - s.depth;
}
// A shaft's FLOOR is open on every spanned level EXCEPT the bottom (you LAND there); its
// CEILING is open on every spanned level EXCEPT the top (you FALL IN there). (Mirror chunk.cpp.)
inline bool shaft_floor_open(const ShaftSpec& s, int32_t level) {
    return s.present && level >  s.top_level - s.depth && level <= s.top_level;
}
inline bool shaft_ceil_open(const ShaftSpec& s, int32_t level) {
    return s.present && level >= s.top_level - s.depth && level <  s.top_level;
}

// M30 (live descent): the per-cell floor/ceiling opening tests, factored so GenerateChunk
// (which precomputes the stair/shaft specs once per chunk) and the live-walk collision
// (floor_hole_at, per cell) share ONE definition -- no drift. A floor cell opens iff a
// DOWN-stair from the level below lands here, or a passing shaft is floor-open at this level;
// the ceiling mirror: an UP-stair leaves here, or a shaft is ceiling-open here. (cell indices
// are 0..G-1 within the chunk.) Pure boolean shape -- evaluation order matches chunk.cpp so the
// floor/ceiling mesh stays bit-identical after the extraction.
inline bool floor_open_in_cell(const StairSpec& dn_stair, const ShaftSpec& shaft,
                               bool shaft_floor_is_open, int cell_i, int cell_j) {
    return (dn_stair.present && dn_stair.cell_i == cell_i && dn_stair.cell_j == cell_j)
        || (shaft_floor_is_open && shaft.cell_i == cell_i && shaft.cell_j == cell_j);
}
inline bool ceil_open_in_cell(const StairSpec& up_stair, const ShaftSpec& shaft,
                              bool shaft_ceil_is_open, int cell_i, int cell_j) {
    return (up_stair.present && up_stair.cell_i == cell_i && up_stair.cell_j == cell_j)
        || (shaft_ceil_is_open && shaft.cell_i == cell_i && shaft.cell_j == cell_j);
}
// Standalone per-cell predicates (recompute the specs internally) -- the SINGLE SOURCE OF
// TRUTH the live-walk collision skips floor cells with, so the wanderer falls through exactly
// the openings GenerateChunk cuts in the floor mesh (down-stair holes + shaft voids). Pure/total
// (hash evals only; one stair_at + one shaft_at). Used off the sim hash (presentation/interaction).
bool floor_hole_at(uint64_t world_seed, int32_t level, int64_t cx, int64_t cz, int cell_i, int cell_j);
bool ceiling_hole_at(uint64_t world_seed, int32_t level, int64_t cx, int64_t cz, int cell_i, int cell_j);

}  // namespace br::gen
