// gen/chunk.cpp — Level-0 chunk geometry (M4): builds floor + maze walls from
// the connected layout (gen/layout.h) into render vertices + collision AABBs.
#include "contracts/chunk_gen_v1.h"

#include <cstring>

#include "core/rng.h"
#include "gen/layout.h"

namespace br::contracts {

namespace {
constexpr float kWallThick = 0.3f;   // wall thickness (m)
constexpr float kWallHeight = 3.0f;  // wall + room height (m)

void push_vertex(std::vector<ChunkVertex>& v, float x, float y, float z,
                 const float n[3], const float col[3]) {
    ChunkVertex cv;
    cv.pos[0] = x; cv.pos[1] = y; cv.pos[2] = z;
    cv.nrm[0] = n[0]; cv.nrm[1] = n[1]; cv.nrm[2] = n[2];
    cv.color[0] = col[0]; cv.color[1] = col[1]; cv.color[2] = col[2];
    v.push_back(cv);
}

void push_quad(std::vector<ChunkVertex>& v, const float a[3], const float b[3],
               const float c[3], const float d[3], const float n[3], const float col[3]) {
    push_vertex(v, a[0], a[1], a[2], n, col);
    push_vertex(v, b[0], b[1], b[2], n, col);
    push_vertex(v, c[0], c[1], c[2], n, col);
    push_vertex(v, a[0], a[1], a[2], n, col);
    push_vertex(v, c[0], c[1], c[2], n, col);
    push_vertex(v, d[0], d[1], d[2], n, col);
}

void push_box(std::vector<ChunkVertex>& v, float x0, float y0, float z0,
              float x1, float y1, float z1, const float col[3]) {
    const float c000[3]={x0,y0,z0}, c001[3]={x0,y0,z1}, c010[3]={x0,y1,z0}, c011[3]={x0,y1,z1};
    const float c100[3]={x1,y0,z0}, c101[3]={x1,y0,z1}, c110[3]={x1,y1,z0}, c111[3]={x1,y1,z1};
    const float nxn[3]={-1,0,0}, nxp[3]={1,0,0}, nyp[3]={0,1,0}, nzn[3]={0,0,-1}, nzp[3]={0,0,1};
    push_quad(v, c000, c001, c011, c010, nxn, col);
    push_quad(v, c100, c110, c111, c101, nxp, col);
    push_quad(v, c010, c011, c111, c110, nyp, col);  // top (visible from above)
    push_quad(v, c000, c010, c110, c100, nzn, col);
    push_quad(v, c001, c101, c111, c011, nzp, col);
}

void add_wall(ChunkData& c, float x0, float z0, float x1, float z1, const float col[3]) {
    push_box(c.vertices, x0, 0.0f, z0, x1, kWallHeight, z1, col);
    BoxInstance bi;
    bi.mn[0] = x0; bi.mn[1] = 0.0f; bi.mn[2] = z0;
    bi.mx[0] = x1; bi.mx[1] = kWallHeight; bi.mx[2] = z1;
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

    // Per-chunk floor tint (muted backrooms palette); walls a darker shade.
    br::core::Pcg64 rng(gen::chunk_seed(world_seed, key));
    const float floor_col[3] = {
        0.58f + 0.22f * static_cast<float>(rng.next_double()),
        0.52f + 0.20f * static_cast<float>(rng.next_double()),
        0.26f + 0.14f * static_cast<float>(rng.next_double()),
    };
    const float wall_col[3] = { floor_col[0] * 0.45f, floor_col[1] * 0.45f, floor_col[2] * 0.40f };
    const float up[3] = {0.0f, 1.0f, 0.0f};

    // Floor grid (world coords; boundary verts align with neighbours -> no seam).
    for (int i = 0; i < G; ++i) {
        for (int j = 0; j < G; ++j) {
            const float x0 = ox + static_cast<float>(i) * cs;
            const float x1 = ox + static_cast<float>(i + 1) * cs;
            const float z0 = oz + static_cast<float>(j) * cs;
            const float z1 = oz + static_cast<float>(j + 1) * cs;
            const float a[3]={x0,0.0f,z0}, b[3]={x1,0.0f,z0}, d[3]={x1,0.0f,z1}, e[3]={x0,0.0f,z1};
            push_quad(c.vertices, a, b, d, e, up, floor_col);
        }
    }

    // Vertical walls (on x-lines xi, spanning z-cell j).
    for (int xi = 0; xi <= G; ++xi) {
        for (int j = 0; j < G; ++j) {
            if (!L.vwall[xi][j]) continue;
            const float xc = ox + static_cast<float>(xi) * cs;
            const float z0 = oz + static_cast<float>(j) * cs;
            const float z1 = oz + static_cast<float>(j + 1) * cs;
            add_wall(c, xc - t, z0, xc + t, z1, wall_col);
        }
    }
    // Horizontal walls (on z-lines zj, spanning x-cell i).
    for (int i = 0; i < G; ++i) {
        for (int zj = 0; zj <= G; ++zj) {
            if (!L.hwall[i][zj]) continue;
            const float zc = oz + static_cast<float>(zj) * cs;
            const float x0 = ox + static_cast<float>(i) * cs;
            const float x1 = ox + static_cast<float>(i + 1) * cs;
            add_wall(c, x0, zc - t, x1, zc + t, wall_col);
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
    }
    return h;
}

bool ValidateChunkGeometry(const ChunkData& c) {
    // Thresholds sit well between the wall thickness (~0.3 m) and the cell size
    // (4 m), so they tolerate the float-precision noise that grows with world
    // distance (far-chunk precision is deferred to camera-relative rendering)
    // while still rejecting genuine fat/stacked-wall bugs.
    const float eps = 0.01f;
    const float thin = gen::kCellSize * 0.5f;  // 2.0 m separator
    for (const BoxInstance& b : c.collision) {
        // Non-degenerate (positive extent on every axis).
        if (b.mx[0] - b.mn[0] <= eps || b.mx[1] - b.mn[1] <= eps || b.mx[2] - b.mn[2] <= eps) return false;
        // Floor-anchored (no floating walls).
        if (b.mn[1] < -eps || b.mn[1] > eps) return false;
        // A wall is thin in exactly one horizontal axis (not a fat block).
        const bool thinX = (b.mx[0] - b.mn[0]) <= thin;
        const bool thinZ = (b.mx[2] - b.mn[2]) <= thin;
        if (thinX == thinZ) return false;
    }
    // No two walls overlap volumetrically beyond a shared corner (a corner is
    // thickness-scale; a duplicate/stacked wall extends a full cell).
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
