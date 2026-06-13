#include "gen/layout.h"

#include "core/rng.h"
#include "gen/biome.h"

namespace br::gen {

namespace {
constexpr int G = kCellsPerChunk;
constexpr uint64_t kTagV = 0x9e3779b97f4a7c15ULL;  // vertical (x-line) edges
constexpr uint64_t kTagH = 0xc2b2ae3d27d4eb4fULL;  // horizontal (z-line) edges

// Doorway cell index for a shared edge identified by (a, b, tag). Both chunks
// that share the edge compute identical (a, b, tag) -> they agree (INV-2).
int door_index(uint64_t seed, int64_t a, int64_t b, uint64_t tag) {
    uint64_t h = seed ^ 0x9e3779b97f4a7c15ULL;
    h ^= static_cast<uint64_t>(a) * 0xff51afd7ed558ccdULL;
    h = (h ^ (h >> 33)) * 0xc4ceb9fe1a85ec53ULL;
    h ^= static_cast<uint64_t>(b) * 0x165667b19e3779f9ULL;
    h = (h ^ (h >> 29)) * tag;
    h ^= h >> 32;
    return static_cast<int>(h % static_cast<uint64_t>(G));
}
}  // namespace

uint64_t chunk_seed(uint64_t world_seed, contracts::ChunkKey key) {
    uint64_t s = world_seed;
    s ^= (static_cast<uint64_t>(key.level) + 1u) * 0x9e3779b97f4a7c15ULL;
    s ^= static_cast<uint64_t>(key.cx) * 0xc2b2ae3d27d4eb4fULL;
    s ^= static_cast<uint64_t>(key.cz) * 0x165667b19e3779f9ULL;
    s ^= s >> 31;
    return s;
}

ChunkLayout generate_layout(uint64_t world_seed, contracts::ChunkKey key) {
    const Biome b = biome_at(world_seed, key.level, key.cx, key.cz);
    return generate_layout_carve(world_seed, key, biome_params(b).carve_ratio);
}

ChunkLayout generate_layout_carve(uint64_t world_seed, contracts::ChunkKey key,
                                  float carve_ratio) {
    ChunkLayout L{};
    for (int xi = 0; xi <= G; ++xi)
        for (int j = 0; j < G; ++j) L.vwall[xi][j] = true;
    for (int i = 0; i < G; ++i)
        for (int zj = 0; zj <= G; ++zj) L.hwall[i][zj] = true;

    br::core::Pcg64 rng(chunk_seed(world_seed, key));

    // Spanning tree (iterative recursive-backtracker): carve walls between cells.
    bool visited[G][G] = {};
    int stackX[G * G + 1];
    int stackZ[G * G + 1];
    int sp = 0;
    stackX[sp] = 0; stackZ[sp] = 0; visited[0][0] = true; ++sp;
    while (sp > 0) {
        const int ci = stackX[sp - 1];
        const int cj = stackZ[sp - 1];
        int ni[4], nj[4], nc = 0;
        if (ci > 0     && !visited[ci - 1][cj]) { ni[nc] = ci - 1; nj[nc] = cj; ++nc; }
        if (ci < G - 1 && !visited[ci + 1][cj]) { ni[nc] = ci + 1; nj[nc] = cj; ++nc; }
        if (cj > 0     && !visited[ci][cj - 1]) { ni[nc] = ci; nj[nc] = cj - 1; ++nc; }
        if (cj < G - 1 && !visited[ci][cj + 1]) { ni[nc] = ci; nj[nc] = cj + 1; ++nc; }
        if (nc == 0) { --sp; continue; }
        const int p = static_cast<int>(rng.bounded(static_cast<uint64_t>(nc)));
        const int a = ni[p], b = nj[p];
        if (a == ci + 1)      L.vwall[ci + 1][cj] = false;  // east
        else if (a == ci - 1) L.vwall[ci][cj] = false;      // west
        else if (b == cj + 1) L.hwall[ci][cj + 1] = false;  // north (+z)
        else                  L.hwall[ci][cj] = false;      // south
        visited[a][b] = true;
        stackX[sp] = a; stackZ[sp] = b; ++sp;
    }

    // Extra openings (loops / room-like openness) on interior walls. The biome's
    // carve ratio sets how open the space feels (tight pipes vs open garage);
    // removing walls atop the spanning tree never breaks connectivity.
    const double carve = static_cast<double>(carve_ratio);
    for (int xi = 1; xi < G; ++xi)
        for (int j = 0; j < G; ++j)
            if (rng.next_double() < carve) L.vwall[xi][j] = false;
    for (int i = 0; i < G; ++i)
        for (int zj = 1; zj < G; ++zj)
            if (rng.next_double() < carve) L.hwall[i][zj] = false;

    // Edge doorways from shared-edge hashes (adjacent chunks agree).
    L.door_left   = door_index(world_seed, key.cx,     key.cz,     kTagV);
    L.door_right  = door_index(world_seed, key.cx + 1, key.cz,     kTagV);
    L.door_bottom = door_index(world_seed, key.cx,     key.cz,     kTagH);
    L.door_top    = door_index(world_seed, key.cx,     key.cz + 1, kTagH);
    L.vwall[0][L.door_left]   = false;
    L.vwall[G][L.door_right]  = false;
    L.hwall[L.door_bottom][0] = false;
    L.hwall[L.door_top][G]    = false;

    return L;
}

void build_stairwell(float x0, float z0, int32_t top_level,
                     std::vector<contracts::BoxInstance>& out) {
    const float topY = contracts::level_base_y(top_level);
    const float botY = contracts::level_base_y(top_level - 1);
    const int steps = 8;
    const float run = 1.5f;  // horizontal depth per step
    const float drop = (topY - botY) / static_cast<float>(steps);  // 0.5 m per step
    for (int k = 0; k < steps; ++k) {
        const float sx0 = x0 + static_cast<float>(k) * run;
        const float top = topY - drop * static_cast<float>(k + 1);  // this step's top surface
        contracts::BoxInstance b;
        b.mn[0] = sx0;        b.mn[1] = botY - 1.0f; b.mn[2] = z0;
        b.mx[0] = sx0 + run;  b.mx[1] = top;         b.mx[2] = z0 + kStairWidth;
        out.push_back(b);
    }
}

bool validate_connectivity(const ChunkLayout& L) {
    bool visited[G][G] = {};
    int stackX[G * G + 1];
    int stackZ[G * G + 1];
    int sp = 0, count = 0;
    stackX[sp] = 0; stackZ[sp] = 0; visited[0][0] = true; ++sp;
    while (sp > 0) {
        const int ci = stackX[sp - 1];
        const int cj = stackZ[sp - 1];
        --sp;
        ++count;
        if (ci < G - 1 && !L.vwall[ci + 1][cj] && !visited[ci + 1][cj]) { visited[ci + 1][cj] = true; stackX[sp] = ci + 1; stackZ[sp] = cj; ++sp; }
        if (ci > 0     && !L.vwall[ci][cj]     && !visited[ci - 1][cj]) { visited[ci - 1][cj] = true; stackX[sp] = ci - 1; stackZ[sp] = cj; ++sp; }
        if (cj < G - 1 && !L.hwall[ci][cj + 1] && !visited[ci][cj + 1]) { visited[ci][cj + 1] = true; stackX[sp] = ci; stackZ[sp] = cj + 1; ++sp; }
        if (cj > 0     && !L.hwall[ci][cj]     && !visited[ci][cj - 1]) { visited[ci][cj - 1] = true; stackX[sp] = ci; stackZ[sp] = cj - 1; ++sp; }
    }
    return count == G * G;
}

}  // namespace br::gen
