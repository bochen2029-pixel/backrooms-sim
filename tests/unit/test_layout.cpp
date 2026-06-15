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

TEST_CASE("vertical connectivity: no floor is ever sealed across a stack slab (M27)", "[m27][stairs]") {
    using br::gen::validate_vertical_connectivity;
    // Over a slab of levels x chunks, every floor must reach every other: horizontal
    // doorways always link, and the per-superblock up-stair backstop guarantees a vertical
    // link out of every floor within bounded distance (radius >= kStairSuperblock).
    for (uint64_t seed : { 1u, 7u, 42u, 0x57A1Bu, 123u }) {
        INFO("seed " << seed);
        REQUIRE(validate_vertical_connectivity(seed, -5, 5, 6));   // 11 floors, 13x13 chunks
    }
    REQUIRE(validate_vertical_connectivity(99u, 0, 12, 5));        // a tall upward band
    REQUIRE(validate_vertical_connectivity(99u, -12, 0, 5));       // and a deep downward band

    // The validator is not vacuous: a single-chunk column (radius 0, no superblock backstop
    // in view) is NOT guaranteed connected, so for at least one seed it must report sealed.
    bool sawSealed = false;
    for (uint64_t seed = 1; seed <= 40 && !sawSealed; ++seed)
        if (!validate_vertical_connectivity(seed, 0, 8, 0)) sawSealed = true;
    REQUIRE(sawSealed);
}

TEST_CASE("shaft_at: a rare, deep, deterministic vertical void (M30)", "[m30][shafts]") {
    const uint64_t seed = 0x5AF7u;
    const int G = br::gen::kCellsPerChunk;
    long present = 0, total = 0;
    for (int64_t cx = -100; cx < 100; ++cx)
        for (int64_t cz = -100; cz < 100; ++cz) {
            const br::gen::ShaftSpec s = br::gen::shaft_at(seed, cx, cz);
            ++total;
            if (!s.present) continue;
            ++present;
            REQUIRE(s.cell_i >= 0); REQUIRE(s.cell_i < G);
            REQUIRE(s.cell_j >= 0); REQUIRE(s.cell_j < G);
            REQUIRE(s.depth >= br::gen::kShaftDepthMin);
            REQUIRE(s.depth <= br::gen::kShaftDepthMax);
            REQUIRE(s.top_level >= -br::gen::kShaftLevelBand);
            REQUIRE(s.top_level <= br::gen::kShaftLevelBand);
            // The void spans EXACTLY [top - depth, top] -- you fall in at top, land on (top - depth).
            REQUIRE(br::gen::shaft_passes(s, s.top_level));
            REQUIRE(br::gen::shaft_passes(s, s.top_level - s.depth));
            REQUIRE_FALSE(br::gen::shaft_passes(s, s.top_level + 1));
            REQUIRE_FALSE(br::gen::shaft_passes(s, s.top_level - s.depth - 1));
        }
    // VERY RARE: they exist, but far below the stair density (~1/13). ~1/1500 here.
    INFO("present " << present << " / " << total);
    REQUIRE(present > 0);
    const double frac = static_cast<double>(present) / static_cast<double>(total);
    REQUIRE(frac < 0.005);

    // DETERMINISTIC, and a different seed relocates the shafts.
    const br::gen::ShaftSpec a = br::gen::shaft_at(seed, 7, -3);
    const br::gen::ShaftSpec b = br::gen::shaft_at(seed, 7, -3);
    REQUIRE(a.present == b.present);
    REQUIRE(a.top_level == b.top_level);
    REQUIRE(a.depth == b.depth);
    bool variesBySeed = false;
    for (int64_t cx = -100; cx < 100 && !variesBySeed; ++cx)
        for (int64_t cz = -100; cz < 100 && !variesBySeed; ++cz)
            if (br::gen::shaft_at(seed, cx, cz).present != br::gen::shaft_at(seed ^ 0x9E37u, cx, cz).present)
                variesBySeed = true;
    REQUIRE(variesBySeed);
}
