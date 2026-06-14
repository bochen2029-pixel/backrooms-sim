// test_gamepad — M16 gamepad -> InputCommand mapping: dead zones, stick-to-move/
// look, and the jump button, all as a pure function of a synthetic GamepadState.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "gamepad.h"

using namespace br::app;

namespace {
constexpr float kLook = 0.04f;  // look radians per unit deflection (test value)
bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }
}  // namespace

TEST_CASE("a disconnected pad produces no input", "[m16][gamepad]") {
    GamepadState g;          // connected=false
    g.lx = 1.0f; g.ry = 1.0f; g.a = true;
    const auto in = gamepad_to_input(g, kLook);
    REQUIRE(in.move_x == 0.0f);
    REQUIRE(in.move_z == 0.0f);
    REQUIRE(in.look_yaw == 0.0f);
    REQUIRE(in.buttons == 0);
}

TEST_CASE("a resting stick inside the dead zone yields zero", "[m16][gamepad]") {
    GamepadState g; g.connected = true;
    g.lx = 0.1f; g.ly = -0.05f;   // inside the 0.2 radial dead zone
    g.rx = 0.15f; g.ry = 0.1f;
    const auto in = gamepad_to_input(g, kLook);
    REQUIRE(in.move_x == 0.0f);
    REQUIRE(in.move_z == 0.0f);
    REQUIRE(in.look_yaw == 0.0f);
    REQUIRE(in.look_pitch == 0.0f);
}

TEST_CASE("left stick drives move, right stick drives look, A jumps", "[m16][gamepad]") {
    GamepadState g; g.connected = true;
    g.ly = 1.0f;     // full forward
    g.rx = 1.0f;     // full look-right
    g.a = true;
    const auto in = gamepad_to_input(g, kLook);
    REQUIRE(in.move_z > 0.9f);           // forward
    REQUIRE(approx(in.move_x, 0.0f));
    REQUIRE(in.look_yaw > 0.0f);         // looks right, scaled by kLook
    REQUIRE(in.look_yaw <= kLook + 1e-4f);
    REQUIRE((in.buttons & br::contracts::kButtonJump) != 0);
}

TEST_CASE("dead-zone rescale reaches full deflection at the edge", "[m16][gamepad]") {
    GamepadState g; g.connected = true;
    g.lx = 1.0f;   // full right on the X axis
    const auto in = gamepad_to_input(g, kLook);
    REQUIRE(approx(in.move_x, 1.0f, 1e-3f));   // magnitude 1 survives the rescale
    REQUIRE(approx(in.move_z, 0.0f));

    // Just outside the dead zone the output is small but non-zero (no step).
    GamepadState g2; g2.connected = true; g2.lx = 0.25f;
    const auto in2 = gamepad_to_input(g2, kLook);
    REQUIRE(in2.move_x > 0.0f);
    REQUIRE(in2.move_x < 0.1f);
}
