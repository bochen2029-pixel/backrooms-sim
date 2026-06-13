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
