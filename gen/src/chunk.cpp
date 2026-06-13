// gen/chunk.cpp — pure procedural chunk generation (INV-2). Placeholder M3
// geometry: a world-coordinate grid floor (per-chunk tint) plus a few interior
// posts, enough to expose seams and verticality while the streaming harness is
// validated. Real Level-0 rooms/doorways arrive in M4.
#include "contracts/chunk_gen_v1.h"

#include <cstring>

#include "core/rng.h"

namespace br::contracts {

namespace {

constexpr int kGrid = 4;  // floor cells per chunk edge (cell = 8 m)

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
    push_quad(v, c010, c011, c111, c110, nyp, col);
    push_quad(v, c000, c010, c110, c100, nzn, col);
    push_quad(v, c001, c101, c111, c011, nzp, col);
}

uint64_t seed_for(uint64_t world_seed, ChunkKey key) {
    uint64_t s = world_seed;
    s ^= static_cast<uint64_t>(static_cast<uint64_t>(key.level) + 1u) * 0x9e3779b97f4a7c15ULL;
    s ^= static_cast<uint64_t>(key.cx) * 0xc2b2ae3d27d4eb4fULL;
    s ^= static_cast<uint64_t>(key.cz) * 0x165667b19e3779f9ULL;
    return s;
}

}  // namespace

ChunkData GenerateChunk(uint64_t world_seed, ChunkKey key) {
    ChunkData c;
    c.key = key;
    br::core::Pcg64 rng(seed_for(world_seed, key));

    const float ox = static_cast<float>(key.cx) * kChunkSize;
    const float oz = static_cast<float>(key.cz) * kChunkSize;
    const float cell = kChunkSize / static_cast<float>(kGrid);

    // Per-chunk floor tint (muted backrooms palette) so neighbours differ.
    const float floor_col[3] = {
        0.55f + 0.25f * static_cast<float>(rng.next_double()),
        0.50f + 0.22f * static_cast<float>(rng.next_double()),
        0.24f + 0.16f * static_cast<float>(rng.next_double()),
    };
    const float up[3] = {0.0f, 1.0f, 0.0f};

    // Floor grid in world coordinates (boundary vertices align with neighbours).
    for (int i = 0; i < kGrid; ++i) {
        for (int j = 0; j < kGrid; ++j) {
            const float x0 = ox + static_cast<float>(i) * cell;
            const float x1 = ox + static_cast<float>(i + 1) * cell;
            const float z0 = oz + static_cast<float>(j) * cell;
            const float z1 = oz + static_cast<float>(j + 1) * cell;
            const float a[3]={x0,0.0f,z0}, b[3]={x1,0.0f,z0}, d[3]={x1,0.0f,z1}, e[3]={x0,0.0f,z1};
            push_quad(c.vertices, a, b, d, e, up, floor_col);
        }
    }

    // A few interior posts (never on a chunk boundary -> no cross-chunk dupes).
    const float post_col[3] = {0.30f, 0.28f, 0.20f};
    const uint64_t posts = 2u + rng.bounded(3u);
    for (uint64_t p = 0; p < posts; ++p) {
        const int gi = 1 + static_cast<int>(rng.bounded(static_cast<uint64_t>(kGrid - 1)));
        const int gj = 1 + static_cast<int>(rng.bounded(static_cast<uint64_t>(kGrid - 1)));
        const float px = ox + static_cast<float>(gi) * cell;
        const float pz = oz + static_cast<float>(gj) * cell;
        const float r = 0.4f;
        const float h = 2.0f + 2.0f * static_cast<float>(rng.next_double());
        push_box(c.vertices, px - r, 0.0f, pz - r, px + r, h, pz + r, post_col);
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

}  // namespace br::contracts
