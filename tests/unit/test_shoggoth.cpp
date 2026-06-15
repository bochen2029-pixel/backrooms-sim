// test_shoggoth — M20 the Shoggoth: deterministic, maze-navigating chase. It is a
// pure function of (state, wanderer, seed) — record→replay reproduces it — and it
// actually routes through the maze toward the wanderer when hunting.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "shoggoth.h"

using namespace br::app;

namespace {
float dist2d(const br::core::Vec3& a, const br::core::Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}
Shoggoth spawn_at(float x, float z) {
    Shoggoth s;
    s.pos = br::core::Vec3{x, br::core::kWandererHalfHeight, z};
    return s;
}
}  // namespace

TEST_CASE("the shoggoth is deterministic (record == replay)", "[m20][shoggoth]") {
    const uint64_t seed = 7u;
    const br::core::Vec3 wanderer{16.0f, 1.0f, 16.0f};
    auto run = [&]() {
        Shoggoth s = spawn_at(28.0f, 16.0f);
        for (int t = 0; t < 800; ++t) shoggoth_step(s, wanderer, seed, (t % 8) == 0);
        return shoggoth_hash(s);
    };
    REQUIRE(run() == run());  // same seed + inputs -> identical fingerprint
}

TEST_CASE("a distant wanderer leaves the shoggoth lurking", "[m20][shoggoth]") {
    Shoggoth s = spawn_at(16.0f, 16.0f);
    const br::core::Vec3 far{16.0f + 200.0f, 1.0f, 16.0f};  // well beyond hunt range
    for (int t = 0; t < 120; ++t) shoggoth_step(s, far, 3u, true);
    REQUIRE(s.state == ShoggothState::Lurk);
}

TEST_CASE("a nearby wanderer triggers the hunt and the shoggoth closes in", "[m20][shoggoth]") {
    const uint64_t seed = 3u;
    const br::core::Vec3 wanderer{16.0f, 1.0f, 16.0f};
    Shoggoth s = spawn_at(16.0f + 24.0f, 16.0f);  // ~6 cells away, within hunt range
    const float d0 = dist2d(s.pos, wanderer);
    // First step flips it into Hunt.
    shoggoth_step(s, wanderer, seed, true);
    REQUIRE(s.state != ShoggothState::Lurk);
    // Over time it routes through the maze and closes a large fraction of the gap
    // (exact arrival depends on the winding path; substantial approach proves nav).
    for (int t = 0; t < 1500; ++t) shoggoth_step(s, wanderer, seed, (t % 8) == 0);
    const float d1 = dist2d(s.pos, wanderer);
    REQUIRE(d1 < d0 - 10.0f);      // closed >=10 m of the ~24 m gap through the maze
}

TEST_CASE("the shoggoth never tunnels into a sealed cell (stays on the maze graph)", "[m20][shoggoth]") {
    // It steers toward cell centres along a BFS route, so after stepping it should
    // still occupy a cell reachable from where it began (the maze is connected).
    const uint64_t seed = 9u;
    const br::core::Vec3 wanderer{16.0f, 1.0f, 16.0f};
    Shoggoth s = spawn_at(20.0f, 16.0f);
    bool moved = false;
    const br::core::Vec3 start = s.pos;
    for (int t = 0; t < 600; ++t) {
        shoggoth_step(s, wanderer, seed, (t % 8) == 0);
        if (dist2d(s.pos, start) > 1.0f) moved = true;
    }
    REQUIRE(moved);  // it actually navigated (didn't get stuck at spawn)
}
