// test_layout — M4 Level-0 generator gates: connectivity (zero sealed boxes),
// geometry validity, and shared-edge doorway agreement, over many chunks.
#include <catch2/catch_test_macros.hpp>

#include "contracts/chunk_gen_v1.h"
#include "core/rng.h"
#include "gen/layout.h"

namespace contracts = br::contracts;
using br::gen::generate_layout;
using br::gen::validate_connectivity;

namespace {
contracts::ChunkKey rand_key(br::core::Pcg64& r) {
    contracts::ChunkKey k;
    k.level = 0;
    k.cx = static_cast<int64_t>(r.bounded(20000u)) - 10000;
    k.cz = static_cast<int64_t>(r.bounded(20000u)) - 10000;
    return k;
}
}  // namespace

TEST_CASE("every chunk layout is fully connected, zero sealed boxes (10000)", "[gen][m4][connectivity]") {
    br::core::Pcg64 r(0xC04Eu);
    for (int n = 0; n < 10000; ++n) {
        REQUIRE(validate_connectivity(generate_layout(123u, rand_key(r))));
    }
}

TEST_CASE("every chunk geometry is valid (10000)", "[gen][m4][geometry]") {
    br::core::Pcg64 r(0xBEEFu);
    for (int n = 0; n < 10000; ++n) {
        const contracts::ChunkData c = contracts::GenerateChunk(123u, rand_key(r));
        REQUIRE(contracts::ValidateChunkGeometry(c));
        REQUIRE_FALSE(c.collision.empty());
    }
}

TEST_CASE("adjacent chunks agree on shared-edge doorways", "[gen][m4][seam]") {
    for (int64_t cx = -4; cx <= 4; ++cx) {
        for (int64_t cz = -4; cz <= 4; ++cz) {
            const auto a     = generate_layout(77u, contracts::ChunkKey{0, cx, cz});
            const auto right = generate_layout(77u, contracts::ChunkKey{0, cx + 1, cz});
            const auto top   = generate_layout(77u, contracts::ChunkKey{0, cx, cz + 1});
            REQUIRE(a.door_right == right.door_left);   // shared vertical edge agrees
            REQUIRE(a.door_top == top.door_bottom);     // shared horizontal edge agrees
        }
    }
}

TEST_CASE("stair_at: hard per-superblock coverage + sparse + deterministic + per-level (M27)", "[m27][stairs]") {
    const uint64_t seed = 0x57A1Bu;
    const int K = br::gen::kStairSuperblock;
    const int G = br::gen::kCellsPerChunk;

    // (1) THE HARD GUARANTEE: every KxK superblock holds >=1 up-stair, so no floor is
    // vertically sealed and a stair is always within a bounded XZ distance (INV-3 in Z).
    long present = 0, total = 0;
    for (int32_t level = -3; level <= 3; ++level) {
        for (int64_t bx = -6; bx <= 6; ++bx) {
            for (int64_t bz = -6; bz <= 6; ++bz) {
                int inBlock = 0;
                for (int di = 0; di < K; ++di)
                    for (int dj = 0; dj < K; ++dj) {
                        const br::gen::StairSpec s = br::gen::stair_at(seed, level, bx * K + di, bz * K + dj);
                        if (s.present) {
                            ++inBlock; ++present;
                            REQUIRE(s.cell_i >= 0); REQUIRE(s.cell_i < G);
                            REQUIRE(s.cell_j >= 0); REQUIRE(s.cell_j < G);
                        }
                        ++total;
                    }
                INFO("level " << level << " block (" << bx << "," << bz << ")");
                REQUIRE(inBlock >= 1);   // the backstop guarantees it
            }
        }
    }
    // (2) SPARSE but recurrent: far from every chunk, but a healthy density (~1 per block).
    const double frac = static_cast<double>(present) / static_cast<double>(total);
    INFO("present fraction = " << frac);
    REQUIRE(frac > 0.03);
    REQUIRE(frac < 0.40);

    // (3) DETERMINISTIC + the shared-seam contract (L's up-stair == L+1's down-hole query, same fn).
    const br::gen::StairSpec a = br::gen::stair_at(seed, 2, 5, -7);
    const br::gen::StairSpec b = br::gen::stair_at(seed, 2, 5, -7);
    REQUIRE(a.present == b.present);
    REQUIRE(a.cell_i == b.cell_i);
    REQUIRE(a.cell_j == b.cell_j);

    // (4) PER-LEVEL: the stair map varies by floor (Z is non-repeating).
    bool variesByLevel = false;
    for (int64_t cx = 0; cx < 64 && !variesByLevel; ++cx)
        if (br::gen::stair_at(seed, 0, cx, 0).present != br::gen::stair_at(seed, 1, cx, 0).present)
            variesByLevel = true;
    REQUIRE(variesByLevel);
}
