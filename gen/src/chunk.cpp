// gen/chunk.cpp — Level-0 chunk geometry: floor + maze walls + ceiling (with a
// regular fluorescent-tile grid) from the connected layout (gen/layout.h), into
// render vertices (pos/nrm/color/uv/material) + collision AABBs.
#include "contracts/chunk_gen_v1.h"

#include <cstring>

#include "core/rng.h"
#include "gen/biome.h"
#include "gen/layout.h"

namespace br::contracts {

namespace {
constexpr float kWallThick = 0.3f;   // wall thickness (m)
constexpr float kWallHeight = 3.0f;  // wall + ceiling height (m)

void push_vertex(std::vector<ChunkVertex>& v, float x, float y, float z,
                 const float n[3], const float col[3], float u, float w, float mat) {
    ChunkVertex cv;
    cv.pos[0] = x; cv.pos[1] = y; cv.pos[2] = z;
    cv.nrm[0] = n[0]; cv.nrm[1] = n[1]; cv.nrm[2] = n[2];
    cv.color[0] = col[0]; cv.color[1] = col[1]; cv.color[2] = col[2];
    cv.uv[0] = u; cv.uv[1] = w;
    cv.material = mat;
    v.push_back(cv);
}

// Quad a->b->c->d with per-quad UV (0,0),(1,0),(1,1),(0,1).
void push_quad(std::vector<ChunkVertex>& v, const float a[3], const float b[3],
               const float c[3], const float d[3], const float n[3],
               const float col[3], float mat) {
    push_vertex(v, a[0], a[1], a[2], n, col, 0.0f, 0.0f, mat);
    push_vertex(v, b[0], b[1], b[2], n, col, 1.0f, 0.0f, mat);
    push_vertex(v, c[0], c[1], c[2], n, col, 1.0f, 1.0f, mat);
    push_vertex(v, a[0], a[1], a[2], n, col, 0.0f, 0.0f, mat);
    push_vertex(v, c[0], c[1], c[2], n, col, 1.0f, 1.0f, mat);
    push_vertex(v, d[0], d[1], d[2], n, col, 0.0f, 1.0f, mat);
}

void push_box(std::vector<ChunkVertex>& v, float x0, float y0, float z0,
              float x1, float y1, float z1, const float col[3], float mat) {
    const float c000[3]={x0,y0,z0}, c001[3]={x0,y0,z1}, c010[3]={x0,y1,z0}, c011[3]={x0,y1,z1};
    const float c100[3]={x1,y0,z0}, c101[3]={x1,y0,z1}, c110[3]={x1,y1,z0}, c111[3]={x1,y1,z1};
    const float nxn[3]={-1,0,0}, nxp[3]={1,0,0}, nyp[3]={0,1,0}, nzn[3]={0,0,-1}, nzp[3]={0,0,1};
    push_quad(v, c000, c001, c011, c010, nxn, col, mat);
    push_quad(v, c100, c110, c111, c101, nxp, col, mat);
    push_quad(v, c010, c011, c111, c110, nyp, col, mat);  // top
    push_quad(v, c000, c010, c110, c100, nzn, col, mat);
    push_quad(v, c001, c101, c111, c011, nzp, col, mat);
}

void add_wall(ChunkData& c, float x0, float z0, float x1, float z1, const float col[3], float baseY) {
    push_box(c.vertices, x0, baseY, z0, x1, baseY + kWallHeight, z1, col, kMatWallpaper);
    BoxInstance bi;
    bi.mn[0] = x0; bi.mn[1] = baseY; bi.mn[2] = z0;
    bi.mx[0] = x1; bi.mx[1] = baseY + kWallHeight; bi.mx[2] = z1;
    c.collision.push_back(bi);
}
}  // namespace

ChunkData GenerateChunk(uint64_t world_seed, ChunkKey key) {
    ChunkData c;
    c.key = key;
    const gen::ChunkLayout L = gen::generate_layout(world_seed, key);

    const float ox = static_cast<float>(key.cx) * kChunkSize;
    const float oz = static_cast<float>(key.cz) * kChunkSize;
    const float cs = gen::kCellSize;
    const float t = kWallThick * 0.5f;
    const int G = gen::kCellsPerChunk;
    const float H = kWallHeight;

    // Vertical level offset (M7): level 0 -> baseY 0 (unchanged); sublevels (-1)
    // sit below and read dimmer/grimier.
    const float baseY = level_base_y(key.level);
    const float sub = (key.level < 0) ? 0.6f : 1.0f;

    br::core::Pcg64 rng(gen::chunk_seed(world_seed, key));
    const gen::BiomeParams bp = gen::biome_params(gen::biome_at(world_seed, key.level, key.cx, key.cz));
    const float floor_col[3] = {
        (0.58f + 0.22f * static_cast<float>(rng.next_double())) * bp.floor_tint[0] * sub,
        (0.52f + 0.20f * static_cast<float>(rng.next_double())) * bp.floor_tint[1] * sub,
        (0.26f + 0.14f * static_cast<float>(rng.next_double())) * bp.floor_tint[2] * sub,
    };
    const float wall_col[3] = { floor_col[0] * bp.wall_darken, floor_col[1] * bp.wall_darken,
                                floor_col[2] * bp.wall_darken };
    const float ceil_col[3] = { 0.85f, 0.85f, 0.82f };
    const float lamp_col[3] = { 1.0f, 1.0f, 0.97f };
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float down[3] = {0.0f, -1.0f, 0.0f};

    // M27: a stairwell cuts a hole through the shared floor/ceiling seam. An UP-stair
    // (this level -> level+1) leaves through MY ceiling; a stair from the level below
    // (level-1 -> this level) arrives through MY floor. Both sides read the SAME
    // stair_at, so my ceiling hole aligns exactly with the next floor's hole (INV-2).
    const gen::StairSpec upStair = gen::stair_at(world_seed, key.level, key.cx, key.cz);
    const gen::StairSpec dnStair = gen::stair_at(world_seed, key.level - 1, key.cx, key.cz);

    // Floor + ceiling grids (per cell). Ceiling faces down; some tiles fluorescent.
    for (int i = 0; i < G; ++i) {
        for (int j = 0; j < G; ++j) {
            const float x0 = ox + static_cast<float>(i) * cs;
            const float x1 = ox + static_cast<float>(i + 1) * cs;
            const float z0 = oz + static_cast<float>(j) * cs;
            const float z1 = oz + static_cast<float>(j + 1) * cs;
            const bool floorHole = dnStair.present && dnStair.cell_i == i && dnStair.cell_j == j;
            const float f0[3]={x0,baseY,z0}, f1[3]={x1,baseY,z0}, f2[3]={x1,baseY,z1}, f3[3]={x0,baseY,z1};
            if (!floorHole)  // M27: a stair from the level below arrives here -> open the floor
                push_quad(c.vertices, f0, f1, f2, f3, up, floor_col, kMatCarpet);

            const int64_t gi = key.cx * G + i;
            const int64_t gj = key.cz * G + j;
            const bool lamp = is_fluorescent_cell(gi, gj);
            const bool ceilHole = upStair.present && upStair.cell_i == i && upStair.cell_j == j;
            const float cy = baseY + H;
            const float ck0[3]={x0,cy,z0}, ck1[3]={x1,cy,z0}, ck2[3]={x1,cy,z1}, ck3[3]={x0,cy,z1};
            if (!ceilHole)  // M27: an up-stair leaves through here -> open the ceiling
                push_quad(c.vertices, ck0, ck3, ck2, ck1, down,
                          lamp ? lamp_col : ceil_col, lamp ? kMatFluorescent : kMatCeiling);
        }
    }

    // M27: the up-stairwell -- climbable steps + collision filling the up-stair cell.
    // Eight 0.5 m risers across the 4 m cell (a 45-degree run) climb exactly kLevelHeight,
    // so the wanderer ascends through the ceiling hole and steps onto the next floor.
    // Each step is a thin, grounded riser-slab inset from the cell walls -- thin in X
    // (a "wall" to ValidateChunkGeometry), abutting in X (no fat overlap), climbing
    // baseY -> baseY+kLevelHeight. generate_layout carves the stair cell open so the
    // low (-X) end is reachable; the stair cell is skipped by the pillar pass below.
    if (upStair.present) {
        const int N = 8;
        const float ins = 0.3f;  // X inset clears the perimeter walls (no fat overlap)
        const float sx0 = ox + static_cast<float>(upStair.cell_i) * cs + ins;
        const float sxe = ox + static_cast<float>(upStair.cell_i + 1) * cs - ins;
        const float zA = oz + static_cast<float>(upStair.cell_j) * cs + 0.4f;
        const float zB = oz + static_cast<float>(upStair.cell_j + 1) * cs - 0.4f;
        const float run = (sxe - sx0) / static_cast<float>(N);    // ~0.43 m tread
        const float rise = kLevelHeight / static_cast<float>(N);  // 0.5 m riser
        const float step_col[3] = { floor_col[0] * 0.8f, floor_col[1] * 0.8f, floor_col[2] * 0.78f };
        for (int k = 0; k < N; ++k) {
            const float bx0 = sx0 + static_cast<float>(k) * run;
            const float bx1 = sx0 + static_cast<float>(k + 1) * run;
            const float topY = baseY + rise * static_cast<float>(k + 1);
            push_box(c.vertices, bx0, baseY, zA, bx1, topY, zB, step_col, kMatCarpet);
            BoxInstance bi;
            bi.mn[0] = bx0; bi.mn[1] = baseY; bi.mn[2] = zA;
            bi.mx[0] = bx1; bi.mx[1] = topY;  bi.mx[2] = zB;
            c.collision.push_back(bi);
        }
    }

    // Vertical walls (x-lines).
    for (int xi = 0; xi <= G; ++xi) {
        for (int j = 0; j < G; ++j) {
            if (!L.vwall[xi][j]) continue;
            const float xc = ox + static_cast<float>(xi) * cs;
            const float z0 = oz + static_cast<float>(j) * cs;
            const float z1 = oz + static_cast<float>(j + 1) * cs;
            add_wall(c, xc - t, z0, xc + t, z1, wall_col, baseY);
        }
    }
    // Horizontal walls (z-lines).
    for (int i = 0; i < G; ++i) {
        for (int zj = 0; zj <= G; ++zj) {
            if (!L.hwall[i][zj]) continue;
            const float zc = oz + static_cast<float>(zj) * cs;
            const float x0 = ox + static_cast<float>(i) * cs;
            const float x1 = ox + static_cast<float>(i + 1) * cs;
            add_wall(c, x0, zc - t, x1, zc + t, wall_col, baseY);
        }
    }

    // Pillars (set-piece columns: parking garage, pillar halls). A small square
    // column at the cell centre — full-height, collidable — leaving ~1.75 m of
    // clearance on each side, so it never blocks cell-to-cell passage (the cell
    // grid that validate_connectivity flood-fills is unaffected). Only consumed
    // when the biome calls for it, so pillar-free biomes stay bit-identical.
    if (bp.pillar_density > 0.0f) {
        const float ps = 0.25f;  // half-extent -> 0.5 m square column
        const float pcol[3] = { wall_col[0] * 0.85f, wall_col[1] * 0.85f, wall_col[2] * 0.85f };
        for (int i = 0; i < G; ++i) {
            for (int j = 0; j < G; ++j) {
                if (rng.next_double() >= static_cast<double>(bp.pillar_density)) continue;
                if (upStair.present && i == upStair.cell_i && j == upStair.cell_j) continue;  // M27: the stairwell owns this cell
                const float pcx = ox + (static_cast<float>(i) + 0.5f) * cs;
                const float pcz = oz + (static_cast<float>(j) + 0.5f) * cs;
                push_box(c.vertices, pcx - ps, baseY, pcz - ps, pcx + ps, baseY + kWallHeight, pcz + ps,
                         pcol, kMatBaseboard);
                BoxInstance bi;
                bi.mn[0] = pcx - ps; bi.mn[1] = baseY; bi.mn[2] = pcz - ps;
                bi.mx[0] = pcx + ps; bi.mx[1] = baseY + kWallHeight; bi.mx[2] = pcz + ps;
                c.collision.push_back(bi);
            }
        }
    }

    c.content_hash = ChunkContentHash(c);
    return c;
}

uint64_t ChunkContentHash(const ChunkData& c) {
    uint64_t h = 1469598103934665603ULL;
    const uint64_t prime = 1099511628211ULL;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= prime; }
    };
    mix(&c.key.level, sizeof(c.key.level));
    mix(&c.key.cx, sizeof(c.key.cx));
    mix(&c.key.cz, sizeof(c.key.cz));
    for (const ChunkVertex& v : c.vertices) {
        mix(v.pos, sizeof(v.pos));
        mix(v.nrm, sizeof(v.nrm));
        mix(v.color, sizeof(v.color));
        mix(v.uv, sizeof(v.uv));
        mix(&v.material, sizeof(v.material));
    }
    return h;
}

bool ValidateChunkGeometry(const ChunkData& c) {
    const float eps = 0.01f;
    const float thin = gen::kCellSize * 0.5f;  // 2.0 m separator
    const float kPillarMax = 1.0f;             // a pillar is a small square column
    const float baseY = level_base_y(c.key.level);  // floor Y for this chunk's level
    for (const BoxInstance& b : c.collision) {
        const float ex = b.mx[0] - b.mn[0];
        const float ez = b.mx[2] - b.mn[2];
        if (ex <= eps || b.mx[1] - b.mn[1] <= eps || ez <= eps) return false;  // degenerate
        if (b.mn[1] < baseY - eps || b.mn[1] > baseY + eps) return false;      // floating
        const bool thinX = ex <= thin;
        const bool thinZ = ez <= thin;
        // A box is either a WALL (thin in exactly one axis) or a free-standing
        // PILLAR (small + square: thin in BOTH axes, <= 1 m). Anything thin in
        // neither axis is a fat/degenerate block (M7: pillars are valid geometry).
        const bool is_wall = (thinX != thinZ);
        const bool is_pillar = thinX && thinZ && ex <= kPillarMax && ez <= kPillarMax;
        if (!is_wall && !is_pillar) return false;
    }
    for (size_t i = 0; i < c.collision.size(); ++i) {
        for (size_t j = i + 1; j < c.collision.size(); ++j) {
            const BoxInstance& a = c.collision[i];
            const BoxInstance& b = c.collision[j];
            const float xv = (a.mx[0] < b.mx[0] ? a.mx[0] : b.mx[0]) - (a.mn[0] > b.mn[0] ? a.mn[0] : b.mn[0]);
            const float zv = (a.mx[2] < b.mx[2] ? a.mx[2] : b.mx[2]) - (a.mn[2] > b.mn[2] ? a.mn[2] : b.mn[2]);
            if (xv > eps && zv > eps && (xv > thin || zv > thin)) return false;
        }
    }
    return true;
}

}  // namespace br::contracts
