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
std::vector<std::pair<float, float>> edge_at_x(const ChunkData& c, float x) {
    std::vector<std::pair<float, float>> out;
    for (const auto& v : c.vertices) {
        if (v.pos[0] == x) out.emplace_back(v.pos[1], v.pos[2]);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
std::vector<std::pair<float, float>> edge_at_z(const ChunkData& c, float z) {
    std::vector<std::pair<float, float>> out;
    for (const auto& v : c.vertices) {
        if (v.pos[2] == z) out.emplace_back(v.pos[0], v.pos[1]);
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
            REQUIRE(edge_at_x(a, sx) == edge_at_x(bx, sx));  // +X seam
            REQUIRE(edge_at_z(a, sz) == edge_at_z(bz, sz));  // +Z seam
            REQUIRE_FALSE(edge_at_x(a, sx).empty());          // seam is non-trivial
        }
    }
}
