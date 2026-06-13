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
