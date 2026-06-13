// test_chunk — GenerateChunk purity (INV-2): regen-identical hashes and exact
// seam agreement between adjacent chunks (no cracks).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "contracts/chunk_gen_v1.h"
#include "core/rng.h"

namespace contracts = br::contracts;
using contracts::ChunkData;
using contracts::ChunkKey;
using contracts::GenerateChunk;

TEST_CASE("GenerateChunk is bit-identical on regeneration (1000 chunks)", "[gen][chunk]") {
    br::core::Pcg64 r(0x5EEDu);
    for (int n = 0; n < 1000; ++n) {
        ChunkKey k;
        k.level = 0;
        k.cx = static_cast<int64_t>(r.bounded(4000u)) - 2000;
        k.cz = static_cast<int64_t>(r.bounded(4000u)) - 2000;
        const ChunkData a = GenerateChunk(1234u, k);
        const ChunkData b = GenerateChunk(1234u, k);
        REQUIRE(a.content_hash == b.content_hash);
        REQUIRE(a.vertices.size() == b.vertices.size());
        REQUIRE(std::memcmp(a.vertices.data(), b.vertices.data(),
                            a.vertices.size() * sizeof(contracts::ChunkVertex)) == 0);
    }
}

TEST_CASE("distinct keys/seeds produce distinct content", "[gen][chunk]") {
    const ChunkData a = GenerateChunk(1234u, ChunkKey{0, 0, 0});
    REQUIRE(a.content_hash != GenerateChunk(1234u, ChunkKey{0, 1, 0}).content_hash);
    REQUIRE(a.content_hash != GenerateChunk(1234u, ChunkKey{0, 0, 1}).content_hash);
    REQUIRE(a.content_hash != GenerateChunk(9999u, ChunkKey{0, 0, 0}).content_hash);
}

namespace {
// Floor-only edge vertices (y==0, upward normal) — wall side/top verts that
// merely touch the chunk boundary are excluded; the seam invariant is floor
// continuity (no cracks). Adjacent chunks must share these exactly.
bool is_floor(const contracts::ChunkVertex& v) { return v.pos[1] == 0.0f && v.nrm[1] > 0.5f; }

std::vector<float> floor_edge_x(const ChunkData& c, float x) {
    std::vector<float> out;  // z values of floor verts on the x==`x` edge
    for (const auto& v : c.vertices) {
        if (v.pos[0] == x && is_floor(v)) out.push_back(v.pos[2]);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
std::vector<float> floor_edge_z(const ChunkData& c, float z) {
    std::vector<float> out;  // x values of floor verts on the z==`z` edge
    for (const auto& v : c.vertices) {
        if (v.pos[2] == z && is_floor(v)) out.push_back(v.pos[0]);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
}  // namespace

TEST_CASE("adjacent chunk seams match exactly (no cracks)", "[gen][chunk][seam]") {
    for (int64_t cx = -3; cx <= 3; ++cx) {
        for (int64_t cz = -3; cz <= 3; ++cz) {
            const ChunkData a   = GenerateChunk(77u, ChunkKey{0, cx, cz});
            const ChunkData bx  = GenerateChunk(77u, ChunkKey{0, cx + 1, cz});
            const ChunkData bz  = GenerateChunk(77u, ChunkKey{0, cx, cz + 1});
            const float sx = static_cast<float>(cx + 1) * contracts::kChunkSize;
            const float sz = static_cast<float>(cz + 1) * contracts::kChunkSize;
            REQUIRE(floor_edge_x(a, sx) == floor_edge_x(bx, sx));  // +X floor seam
            REQUIRE(floor_edge_z(a, sz) == floor_edge_z(bz, sz));  // +Z floor seam
            REQUIRE_FALSE(floor_edge_x(a, sx).empty());            // seam is non-trivial
        }
    }
}
