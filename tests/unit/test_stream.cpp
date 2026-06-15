// test_stream — chunk ring fills, stays bounded (INV-4), and recenters cleanly.
#include <catch2/catch_test_macros.hpp>

#include "stream/stream_manager.h"

using br::stream::StreamManager;
namespace contracts = br::contracts;

namespace {
int64_t iabs(int64_t v) { return v < 0 ? -v : v; }
}  // namespace

TEST_CASE("stream ring fills to (2r+1)^2 and stays bounded", "[stream]") {
    StreamManager sm(123u, 2, 3u);  // radius 2 -> 5x5 = 25 chunks, 3 workers
    sm.update(contracts::ChunkKey{0, 0, 0});
    sm.wait_idle();
    sm.update(contracts::ChunkKey{0, 0, 0});

    REQUIRE(sm.resident_count() == 25u);
    REQUIRE(sm.pending_count() == 0u);
    for (const auto& rc : sm.resident()) {
        REQUIRE(iabs(rc.key.cx) <= 2);
        REQUIRE(iabs(rc.key.cz) <= 2);
        REQUIRE(rc.vertex_count > 0u);
    }
}

TEST_CASE("recentering evicts the old ring; residency stays bounded", "[stream]") {
    StreamManager sm(123u, 2, 4u);
    sm.update(contracts::ChunkKey{0, 0, 0});
    sm.wait_idle();
    sm.update(contracts::ChunkKey{0, 0, 0});
    REQUIRE(sm.resident_count() == 25u);

    sm.update(contracts::ChunkKey{0, 100, 100});
    sm.wait_idle();
    sm.update(contracts::ChunkKey{0, 100, 100});

    REQUIRE(sm.resident_count() == 25u);          // still bounded after moving far
    for (const auto& rc : sm.resident()) {
        REQUIRE(iabs(rc.key.cx - 100) <= 2);
        REQUIRE(iabs(rc.key.cz - 100) <= 2);
    }
    REQUIRE(sm.generated_total() >= 50u);          // generated both rings
}

TEST_CASE("two-level residency: a second adjacent ring streams in + stays bounded (M28)", "[stream][m28]") {
    StreamManager sm(123u, 2, 4u);  // radius 2 -> 25 chunks per level
    // Stream level 0 + the floor above (level 1): both rings resident, bounded at 2 x 25.
    sm.update(contracts::ChunkKey{0, 0, 0}, 1);
    sm.wait_idle();
    sm.update(contracts::ChunkKey{0, 0, 0}, 1);
    REQUIRE(sm.resident_count() == 50u);
    int n0 = 0, n1 = 0;
    for (const auto& rc : sm.resident()) {
        REQUIRE(iabs(rc.key.cx) <= 2);
        REQUIRE(iabs(rc.key.cz) <= 2);
        REQUIRE(rc.vertex_count > 0u);
        if (rc.key.level == 0) ++n0;
        else if (rc.key.level == 1) ++n1;
    }
    REQUIRE(n0 == 25);
    REQUIRE(n1 == 25);   // the floor above is resident too -> see-through + seamless climb

    // Switching the adjacent level (now looking DOWN) evicts the old one; still bounded.
    sm.update(contracts::ChunkKey{0, 0, 0}, -1);
    sm.wait_idle();
    sm.update(contracts::ChunkKey{0, 0, 0}, -1);
    REQUIRE(sm.resident_count() == 50u);
    int up = 0;
    for (const auto& rc : sm.resident())
        if (rc.key.level == 1) ++up;
    REQUIRE(up == 0);    // level +1 fully evicted when the adjacent level switched

    // Backward-compatible: the single-arg update is exactly single-level (the prior path).
    sm.update(contracts::ChunkKey{0, 0, 0});
    sm.wait_idle();
    sm.update(contracts::ChunkKey{0, 0, 0});
    REQUIRE(sm.resident_count() == 25u);
}

TEST_CASE("range residency: a band of floors streams in for the abyss + stays bounded (M30)", "[stream][m30]") {
    StreamManager sm(123u, 2, 4u);  // radius 2 -> 25 chunks per level
    // Look DOWN a shaft: stream the band [-2, 0] (3 floors) -> 3 x 25, all within the ring.
    sm.update(contracts::ChunkKey{0, 0, 0}, -2, 0);
    sm.wait_idle();
    sm.update(contracts::ChunkKey{0, 0, 0}, -2, 0);
    REQUIRE(sm.resident_count() == 75u);
    int per[3] = {0, 0, 0};
    for (const auto& rc : sm.resident()) {
        REQUIRE(iabs(rc.key.cx) <= 2);
        REQUIRE(iabs(rc.key.cz) <= 2);
        REQUIRE(rc.key.level >= -2);
        REQUIRE(rc.key.level <= 0);
        ++per[rc.key.level + 2];
    }
    REQUIRE(per[0] == 25);   // level -2 (deepest)
    REQUIRE(per[1] == 25);   // level -1
    REQUIRE(per[2] == 25);   // level 0

    // Closing the band back to one floor evicts the abyss; residency stays bounded.
    sm.update(contracts::ChunkKey{0, 0, 0}, 0, 0);
    sm.wait_idle();
    sm.update(contracts::ChunkKey{0, 0, 0}, 0, 0);
    REQUIRE(sm.resident_count() == 25u);
}
