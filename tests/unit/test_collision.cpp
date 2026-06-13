// test_collision — capsule(AABB-proxy)-vs-AABB collision exit gates (M2):
// no wall penetration at any speed, sliding preserves tangential velocity,
// no floor tunneling under large per-tick movement.
#include <catch2/catch_test_macros.hpp>

#include "core/world.h"

using namespace br::core;
namespace contracts = br::contracts;

namespace {
const Vec3 kHe{kWandererRadius, kWandererHalfHeight, kWandererRadius};

bool penetrates(Vec3 pos) {
    const Aabb w = box_at(pos, kHe);
    for (const Aabb& b : test_room()) {
        if (overlaps(w, b)) return true;
    }
    return false;
}
}  // namespace

TEST_CASE("no wall penetration even at extreme speed", "[collision]") {
    for (float speed : {10.0f, 50.0f, 200.0f, 1000.0f}) {
        Vec3 pos{0.0f, kWandererHalfHeight + 0.02f, 0.0f};
        Vec3 vel{speed, 0.0f, 0.0f};
        bool grounded = false;
        // A handful of ticks driving hard into the +X wall.
        for (int i = 0; i < 30; ++i) {
            vel.x = speed;  // keep pushing
            pos = move_and_collide(pos, kHe, vel, kTickDt, test_room(), grounded);
        }
        INFO("speed = " << speed << "  final x = " << pos.x);
        REQUIRE_FALSE(penetrates(pos));
        REQUIRE(pos.x < 6.0f);   // stopped before the +X wall inner face
    }
}

TEST_CASE("sliding preserves tangential velocity", "[collision]") {
    // Near the +X wall: move +X (gets blocked) and +Z (stays free). Push for a
    // few ticks until the wall is reached, then check the resolved velocity.
    Vec3 pos{5.5f, kWandererHalfHeight + 0.02f, 0.0f};
    Vec3 vel{};
    bool grounded = false;
    for (int i = 0; i < 8; ++i) {
        vel = Vec3{10.0f, 0.0f, 7.0f};
        pos = move_and_collide(pos, kHe, vel, kTickDt, test_room(), grounded);
    }
    REQUIRE(vel.x == 0.0f);          // normal component killed by the wall
    REQUIRE(vel.z == 7.0f);          // tangential component preserved exactly
    REQUIRE_FALSE(penetrates(pos));
    REQUIRE(pos.z > 0.0f);           // actually slid along the wall
    REQUIRE(pos.x < 6.0f);
}

TEST_CASE("no floor tunneling under large downward velocity", "[collision]") {
    // Even at absurd downward speeds the capsule must never pass through the
    // floor; it should settle on it. Re-apply the velocity each tick and assert
    // it is never below the floor at any step.
    for (float down : {20.0f, 100.0f, 500.0f, 2000.0f}) {
        Vec3 pos{0.0f, 1.5f, 0.0f};
        Vec3 vel{};
        bool grounded = false;
        for (int i = 0; i < 120; ++i) {
            vel = Vec3{0.0f, -down, 0.0f};
            pos = move_and_collide(pos, kHe, vel, kTickDt, test_room(), grounded);
            INFO("down = " << down << "  step " << i << "  y = " << pos.y);
            REQUIRE(pos.y >= kWandererHalfHeight - 0.02f);  // never below floor
            REQUIRE_FALSE(penetrates(pos));
        }
        REQUIRE(grounded);
        REQUIRE(vel.y == 0.0f);
    }
}

TEST_CASE("wanderer rests on the floor over time", "[collision]") {
    WorldState s(123u);
    contracts::InputCommand idle{};
    for (int i = 0; i < 360; ++i) tick(s, idle);  // 3 seconds, no input
    REQUIRE(s.wanderer.on_ground);
    REQUIRE(s.wanderer.pos.y > kWandererHalfHeight - 0.05f);
    REQUIRE(s.wanderer.pos.y < kWandererHalfHeight + 0.10f);
    REQUIRE_FALSE(penetrates(s.wanderer.pos));
}
