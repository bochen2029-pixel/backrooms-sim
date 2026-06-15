// test_headbob — M18 realistic walking: the head-bob curve is a pure, bounded,
// figure-8 sine of the walked distance (walk vs run), and the kButtonRun modifier
// raises the sim's move speed deterministically.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "head_bob.h"

#include "core/world.h"
#include "contracts/replay_v1.h"

using namespace br;

namespace {
constexpr float kWalk = 4.0f;
constexpr float kRun = 6.8f;
}  // namespace

TEST_CASE("a stopped wanderer has no head-bob", "[m18][headbob]") {
    const app::BobOffset b = app::head_bob(123.4f, 0.0f, kWalk, kRun);
    REQUIRE(b.dy == 0.0f);
    REQUIRE(b.dx == 0.0f);
}

TEST_CASE("the head only ever dips (dy <= 0) and the bob is bounded", "[m18][headbob]") {
    float min_dy = 0.0f, max_absdx = 0.0f, min_dy_run = 0.0f, max_absdx_run = 0.0f;
    for (int i = 0; i < 4000; ++i) {
        const float d = i * 0.01f;
        const app::BobOffset w = app::head_bob(d, kWalk, kWalk, kRun);
        const app::BobOffset r = app::head_bob(d, kRun, kWalk, kRun);
        REQUIRE(w.dy <= 1e-6f);     // never rises above neutral
        REQUIRE(r.dy <= 1e-6f);
        min_dy = std::min(min_dy, w.dy);
        max_absdx = std::max(max_absdx, std::fabs(w.dx));
        min_dy_run = std::min(min_dy_run, r.dy);
        max_absdx_run = std::max(max_absdx_run, std::fabs(r.dx));
    }
    // Walk amplitudes within their configured caps; running bobs harder than walking.
    REQUIRE(min_dy >= -0.051f);             // walk vertical amp ~0.05 m
    REQUIRE(max_absdx <= 0.031f);           // walk lateral amp ~0.03 m
    REQUIRE(min_dy_run < min_dy);           // run dips deeper
    REQUIRE(max_absdx_run > max_absdx);     // run sways wider
}

TEST_CASE("the bob is continuous and deterministic", "[m18][headbob]") {
    // Same inputs -> identical output (pure).
    REQUIRE(app::head_bob(10.0f, kWalk, kWalk, kRun).dy == app::head_bob(10.0f, kWalk, kWalk, kRun).dy);
    // No discontinuity: a tiny step in distance moves the bob only a little.
    float prev_y = app::head_bob(0.0f, kWalk, kWalk, kRun).dy;
    for (int i = 1; i < 2000; ++i) {
        const float y = app::head_bob(i * 0.005f, kWalk, kWalk, kRun).dy;
        REQUIRE(std::fabs(y - prev_y) < 0.01f);
        prev_y = y;
    }
}

TEST_CASE("vertical bob has two dips per stride cycle (footfall cadence)", "[m18][headbob]") {
    // dy = -A/2 (1 - cos(2*phase)); over one phase cycle (2*pi) there are two troughs.
    // cycles/m = 0.31 at walk -> one phase cycle per 1/0.31 m. Count minima over it.
    const float cycle_m = 1.0f / 0.31f;
    int dips = 0;
    const int N = 600;
    float pm = 0.0f, pp = 0.0f;  // prev value, prev-prev
    for (int i = 0; i <= N; ++i) {
        const float d = (cycle_m) * (i / static_cast<float>(N));
        const float y = app::head_bob(d, kWalk, kWalk, kRun).dy;
        if (i >= 2 && pm < pp && pm < y) ++dips;  // local minimum at the middle sample
        pp = pm; pm = y;
    }
    REQUIRE(dips == 2);
}

TEST_CASE("kButtonRun raises move speed deterministically (odometer)", "[m18][run]") {
    using namespace br::core;
    const std::vector<Aabb>& ground = open_ground();
    auto walk_dist = [&](uint8_t buttons) {
        WorldState s(7u);
        s.wanderer.pos = Vec3{16.0f, kWandererHalfHeight + 0.02f, 16.0f};
        contracts::InputCommand in{};
        in.move_z = 1.0f;
        in.buttons = buttons;
        for (int i = 0; i < 120; ++i) tick(s, in, ground);  // 1 s at 120 Hz
        return s.odometer;
    };
    const float walk = walk_dist(0);
    const float run = walk_dist(contracts::kButtonRun);
    REQUIRE(run > walk * 1.5f);                 // running is ~1.7x walk
    REQUIRE(walk_dist(0) == walk);              // deterministic across runs
    REQUIRE(walk_dist(contracts::kButtonRun) == run);
}
