// test_biome — M7 biome field: pure/deterministic, low-frequency (contiguous
// coarse cells), and the realized distribution over 100k chunks matches the
// designed weights within +/- 2 % (the M7 statistical gate).
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "contracts/chunk_gen_v1.h"
#include "gen/biome.h"
#include "gen/layout.h"

using namespace br::gen;

TEST_CASE("biome_at is deterministic and pure", "[m7][biome]") {
    for (int64_t cx = -20; cx <= 20; ++cx) {
        for (int64_t cz = -20; cz <= 20; ++cz) {
            REQUIRE(biome_at(777u, 0, cx, cz) == biome_at(777u, 0, cx, cz));
        }
    }
    // Different seeds generally disagree somewhere.
    bool differ = false;
    for (int64_t cx = 0; cx < 30 && !differ; ++cx)
        for (int64_t cz = 0; cz < 30 && !differ; ++cz)
            if (biome_at(1u, 0, cx, cz) != biome_at(2u, 0, cx, cz)) differ = true;
    REQUIRE(differ);
}

TEST_CASE("biomes are low-frequency: a coarse 3x3 cell shares one biome", "[m7][biome]") {
    // The coarse cell is K=3 chunks; the 3x3 block based at a multiple of 3 must
    // be a single biome (contiguous regions, not per-chunk noise).
    const uint64_t seed = 42u;
    for (int64_t bx = -4; bx <= 4; ++bx) {
        for (int64_t bz = -4; bz <= 4; ++bz) {
            const Biome b = biome_at(seed, 0, bx * 3, bz * 3);
            for (int dx = 0; dx < 3; ++dx)
                for (int dz = 0; dz < 3; ++dz)
                    REQUIRE(biome_at(seed, 0, bx * 3 + dx, bz * 3 + dz) == b);
        }
    }
}

TEST_CASE("biome distribution over 100k chunks is within 2% of the designed weights",
          "[m7][biome]") {
    const uint64_t seed = 1234u;
    const int64_t N = 320;  // 320*320 = 102,400 chunks
    long counts[kBiomeCount] = {0};
    long total = 0;
    for (int64_t cx = 0; cx < N; ++cx) {
        for (int64_t cz = 0; cz < N; ++cz) {
            counts[static_cast<int>(biome_at(seed, 0, cx, cz))]++;
            ++total;
        }
    }
    REQUIRE(total >= 100000);
    for (int b = 0; b < kBiomeCount; ++b) {
        const double frac = static_cast<double>(counts[b]) / static_cast<double>(total);
        const double want = static_cast<double>(biome_weight_pct(static_cast<Biome>(b))) / 100.0;
        INFO("biome " << biome_name(static_cast<Biome>(b)) << " frac=" << frac << " want=" << want);
        REQUIRE(std::fabs(frac - want) <= 0.02);
    }
}

TEST_CASE("every biome layout is fully connected over 10000 chunks (M7 gate)", "[m7][biome]") {
    // Each biome's carve ratio must keep the chunk connected (INV-3): carving
    // openings atop the spanning tree never disconnects, for any ratio.
    for (int bi = 0; bi < kBiomeCount; ++bi) {
        const Biome b = static_cast<Biome>(bi);
        const float carve = biome_params(b).carve_ratio;
        for (int64_t k = 0; k < 10000; ++k) {
            const br::contracts::ChunkKey key{0, k * 131 + 1, k * 17 - 5};
            ChunkLayout L = generate_layout_carve(99u + static_cast<uint64_t>(bi), key, carve);
            INFO("biome " << biome_name(b) << " chunk " << k);
            REQUIRE(validate_connectivity(L));
        }
    }
}

TEST_CASE("every biome's generated geometry is valid, incl pillars (M7 gate)", "[m7][biome]") {
    // Walk the world and validate real GenerateChunk geometry for chunks of each
    // biome (so pillar biomes — parking garage / pillar halls — are covered).
    const uint64_t seed = 31337u;
    int done[kBiomeCount] = {0};
    int remaining = kBiomeCount;
    const int kPer = 2000;
    for (int64_t scan = 0; scan < 4000000 && remaining > 0; ++scan) {
        const int64_t cx = scan % 2048;
        const int64_t cz = scan / 2048;
        const int bi = static_cast<int>(biome_at(seed, 0, cx, cz));
        if (done[bi] >= kPer) continue;
        const br::contracts::ChunkData cd =
            br::contracts::GenerateChunk(seed, br::contracts::ChunkKey{0, cx, cz});
        INFO("biome " << biome_name(static_cast<Biome>(bi)) << " chunk (" << cx << "," << cz << ")");
        REQUIRE(br::contracts::ValidateChunkGeometry(cd));
        if (++done[bi] == kPer) --remaining;
    }
    for (int bi = 0; bi < kBiomeCount; ++bi) {
        INFO("biome " << biome_name(static_cast<Biome>(bi)));
        REQUIRE(done[bi] == kPer);
    }
}

TEST_CASE("level -1 chunks are connected and geometry-valid (cross-level, M7 gate)", "[m7][biome]") {
    // Verticality: sublevel generation must hold the same invariants as level 0
    // (INV-3 connectivity; valid geometry at the level's Y-offset floor).
    const uint64_t seed = 555u;
    // Keep coords in the precision-safe band (geometry is built in world coords;
    // far-chunk float noise is a documented deferral, ADR-022) — same as M4.
    for (int64_t k = 0; k < 4000; ++k) {
        const br::contracts::ChunkKey key{-1, (k % 1000) - 500, (k / 1000) - 2};
        ChunkLayout L = generate_layout(seed, key);
        INFO("level -1 chunk " << k << " (" << key.cx << "," << key.cz << ")");
        REQUIRE(validate_connectivity(L));
        const br::contracts::ChunkData cd = br::contracts::GenerateChunk(seed, key);
        REQUIRE(br::contracts::ValidateChunkGeometry(cd));
    }
}

TEST_CASE("level_from_y inverts level_base_y at the wanderer's stance (M26)", "[m26][level]") {
    // The implicit-level tracking M26 relies on: a point in level L's playable band
    // [base, base+ceiling] resolves back to L, for any floor up or down. level_from_y(0)=0
    // keeps every level-0 path byte-identical.
    for (int32_t L = -64; L <= 64; ++L) {
        const float base = br::contracts::level_base_y(L);
        REQUIRE(br::contracts::level_from_y(base + 0.92f) == L);  // standing on the floor
        REQUIRE(br::contracts::level_from_y(base + 0.10f) == L);  // just above it
        REQUIRE(br::contracts::level_from_y(base + 2.90f) == L);  // head near the ceiling
    }
}

TEST_CASE("floors do not repeat in Z: the biome field varies by level (M26)", "[m26][level]") {
    // Generation already folds `level` into chunk_seed + biome_at, so the same (cx,cz)
    // is a DIFFERENT floor at a different level (the Tegmark-I non-repetition property).
    const uint64_t seed = 4242u;
    const int64_t cx = 7, cz = -3;
    const Biome b0 = biome_at(seed, 0, cx, cz);
    bool varies = false;
    for (int32_t lvl = -32; lvl <= 32 && !varies; ++lvl)
        if (lvl != 0 && biome_at(seed, lvl, cx, cz) != b0) varies = true;
    REQUIRE(varies);  // some neighbouring floor differs from level 0 -> non-repeating in Z
}

TEST_CASE("generation holds INV-3 + valid geometry across many levels (M26)", "[m26][level]") {
    // Every floor up and down obeys the same invariants as level 0 (connectivity + valid
    // geometry at its own level_base_y), so the wanderer can always keep walking on any floor.
    const uint64_t seed = 909u;
    for (int32_t lvl = -16; lvl <= 16; ++lvl) {
        for (int64_t k = 0; k < 64; ++k) {
            const br::contracts::ChunkKey key{lvl, (k % 16) - 8, (k / 16) - 2};
            INFO("level " << lvl << " chunk (" << key.cx << "," << key.cz << ")");
            REQUIRE(validate_connectivity(generate_layout(seed, key)));
            REQUIRE(br::contracts::ValidateChunkGeometry(br::contracts::GenerateChunk(seed, key)));
        }
    }
}
