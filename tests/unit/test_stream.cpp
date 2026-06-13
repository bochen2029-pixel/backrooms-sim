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
