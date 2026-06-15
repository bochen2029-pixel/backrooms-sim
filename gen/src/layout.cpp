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

// M27: a level-folded, tag-separated 64-bit hash for stair placement.
uint64_t stair_hash(uint64_t seed, int32_t level, int64_t a, int64_t b, uint64_t tag) {
    uint64_t h = seed ^ tag;
    h ^= (static_cast<uint64_t>(level) + 1u) * 0x9e3779b97f4a7c15ULL;
    h = (h ^ (h >> 33)) * 0xff51afd7ed558ccdULL;
    h ^= static_cast<uint64_t>(a) * 0xc2b2ae3d27d4eb4fULL;
    h = (h ^ (h >> 29)) * 0xc4ceb9fe1a85ec53ULL;
    h ^= static_cast<uint64_t>(b) * 0x165667b19e3779f9ULL;
    h ^= h >> 32;
    return h;
}
bool stair_density_here(uint64_t seed, int32_t level, int64_t cx, int64_t cz) {
    return (stair_hash(seed, level, cx, cz, 0x53A1ULL) % static_cast<uint64_t>(kStairDensityN)) == 0u;
}
int64_t floordiv_i(int64_t a, int64_t b) {  // round toward -inf (superblock index, neg-safe)
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
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

StairSpec stair_at(uint64_t world_seed, int32_t level, int64_t cx, int64_t cz) {
    StairSpec s;
    auto set_cell = [&]() {
        const uint64_t h = stair_hash(world_seed, level, cx, cz, 0xCE11ULL);
        s.cell_i = static_cast<int>(h % static_cast<uint64_t>(G));
        s.cell_j = static_cast<int>((h / static_cast<uint64_t>(G)) % static_cast<uint64_t>(G));
    };
    // Density scatter: an organic up-stair ~1 per kStairDensityN chunks.
    if (stair_density_here(world_seed, level, cx, cz)) { s.present = true; set_cell(); return s; }
    // Backstop: if NO density stair fell anywhere in this chunk's KxK superblock, one
    // canonical chunk in the block gets one -> every block has >=1 up-stair (INV-3 in Z).
    const int K = kStairSuperblock;
    const int64_t bx = floordiv_i(cx, K), bz = floordiv_i(cz, K);
    for (int di = 0; di < K; ++di)
        for (int dj = 0; dj < K; ++dj)
            if (stair_density_here(world_seed, level, bx * K + di, bz * K + dj)) return s;  // covered elsewhere
    const uint64_t pick = stair_hash(world_seed, level, bx, bz, 0xBACCULL) % static_cast<uint64_t>(K * K);
    const int64_t pcx = bx * K + static_cast<int64_t>(pick % static_cast<uint64_t>(K));
    const int64_t pcz = bz * K + static_cast<int64_t>(pick / static_cast<uint64_t>(K));
    if (cx == pcx && cz == pcz) { s.present = true; set_cell(); }
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

    // M27: keep the up-stair cell reachable at floor level -- open its INTERIOR walls so
    // the staircase is mountable from a neighbour. Carving only removes walls (never
    // disconnects), and perimeter/door walls are untouched, so adjacent chunks still agree
    // on their shared edges (INV-2). The stair cell becomes a small open landing-junction.
    const StairSpec us = stair_at(world_seed, key.level, key.cx, key.cz);
    if (us.present) {
        if (us.cell_i > 0)     L.vwall[us.cell_i][us.cell_j]     = false;  // -X (the low approach)
        if (us.cell_i < G - 1) L.vwall[us.cell_i + 1][us.cell_j] = false;  // +X
        if (us.cell_j > 0)     L.hwall[us.cell_i][us.cell_j]     = false;  // -Z
        if (us.cell_j < G - 1) L.hwall[us.cell_i][us.cell_j + 1] = false;  // +Z
    }

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

bool validate_vertical_connectivity(uint64_t world_seed, int32_t lvl_lo, int32_t lvl_hi, int radius) {
    if (lvl_hi < lvl_lo || radius < 0) return false;
    const int64_t R = radius;
    const int64_t D = 2 * R + 1;
    const int64_t plane = D * D;
    const int64_t total = static_cast<int64_t>(lvl_hi - lvl_lo + 1) * plane;
    auto idx = [&](int32_t L, int64_t cx, int64_t cz) -> int64_t {
        return (static_cast<int64_t>(L - lvl_lo) * D + (cx + R)) * D + (cz + R);
    };
    std::vector<char> seen(static_cast<size_t>(total), 0);
    std::vector<int64_t> stack;
    stack.reserve(static_cast<size_t>(total));
    const int64_t start = idx(lvl_lo, 0, 0);
    seen[static_cast<size_t>(start)] = 1;
    stack.push_back(start);
    int64_t count = 0;
    while (!stack.empty()) {
        const int64_t n = stack.back();
        stack.pop_back();
        ++count;
        const int32_t L = lvl_lo + static_cast<int32_t>(n / plane);
        const int64_t rem = n % plane;
        const int64_t cx = (rem / D) - R;
        const int64_t cz = (rem % D) - R;
        auto visit = [&](int32_t L2, int64_t x2, int64_t z2) {
            const int64_t m = idx(L2, x2, z2);
            if (!seen[static_cast<size_t>(m)]) { seen[static_cast<size_t>(m)] = 1; stack.push_back(m); }
        };
        if (cx > -radius) visit(L, cx - 1, cz);            // horizontal: doorways always link
        if (cx <  radius) visit(L, cx + 1, cz);
        if (cz > -radius) visit(L, cx, cz - 1);
        if (cz <  radius) visit(L, cx, cz + 1);
        if (L < lvl_hi && stair_at(world_seed, L, cx, cz).present)     visit(L + 1, cx, cz);  // up-stair
        if (L > lvl_lo && stair_at(world_seed, L - 1, cx, cz).present) visit(L - 1, cx, cz);  // a stair from below
    }
    return count == total;
}

ShaftSpec shaft_at(uint64_t world_seed, int64_t cx, int64_t cz) {
    ShaftSpec s;
    // Very rare per column (level-independent placement -> tag-separated column hashes).
    if (stair_hash(world_seed, 0, cx, cz, 0x5AF70ULL) % static_cast<uint64_t>(kShaftDensityN) != 0u)
        return s;
    s.present = true;
    const uint64_t hc = stair_hash(world_seed, 0, cx, cz, 0xDEE9ULL);
    s.cell_i = static_cast<int>(hc % static_cast<uint64_t>(G));
    s.cell_j = static_cast<int>((hc / static_cast<uint64_t>(G)) % static_cast<uint64_t>(G));
    const uint64_t hl = stair_hash(world_seed, 0, cx, cz, 0xAB99ULL);
    const uint64_t span = static_cast<uint64_t>(2 * kShaftLevelBand + 1);
    s.top_level = static_cast<int32_t>(hl % span) - kShaftLevelBand;
    s.depth = kShaftDepthMin +
              static_cast<int32_t>((hl / span) % static_cast<uint64_t>(kShaftDepthMax - kShaftDepthMin + 1));
    return s;
}

bool floor_hole_at(uint64_t world_seed, int32_t level, int64_t cx, int64_t cz, int cell_i, int cell_j) {
    const StairSpec dn = stair_at(world_seed, level - 1, cx, cz);  // a stair from below pokes through MY floor
    const ShaftSpec sh = shaft_at(world_seed, cx, cz);
    return floor_open_in_cell(dn, sh, shaft_floor_open(sh, level), cell_i, cell_j);
}

bool ceiling_hole_at(uint64_t world_seed, int32_t level, int64_t cx, int64_t cz, int cell_i, int cell_j) {
    const StairSpec up = stair_at(world_seed, level, cx, cz);      // MY up-stair leaves through MY ceiling
    const ShaftSpec sh = shaft_at(world_seed, cx, cz);
    return ceil_open_in_cell(up, sh, shaft_ceil_open(sh, level), cell_i, cell_j);
}

}  // namespace br::gen
