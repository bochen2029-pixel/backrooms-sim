#pragma once
//
// app/gamepad.h — gamepad -> InputCommand mapping (M16), header-only and PURE.
//
// XInput polling (Win32) fills a plain `GamepadState`; this maps it to the same
// deterministic `contracts::InputCommand` the keyboard/mouse and replay produce —
// so a controller drives the identical tick path, and the mapping is unit-testable
// with synthetic states (no hardware). Radial dead zones keep a resting stick from
// drifting the wanderer; sticks are expected pre-normalised to [-1, 1].
//
#include <cmath>
#include <cstdint>

#include "contracts/replay_v1.h"

namespace br::app {

struct GamepadState {
    bool connected = false;
    float lx = 0.0f, ly = 0.0f;  // left stick  (-1..1); ly +up/forward
    float rx = 0.0f, ry = 0.0f;  // right stick (-1..1); ry +up
    bool a = false;              // jump
    bool start = false;          // pause / menu
    bool run = false;            // hold to run (left trigger / shoulder)
};

// Radial dead zone on a stick: zero inside `dz`, smoothly rescaled outside so the
// usable range still reaches 1.0 (no step at the dead-zone edge).
inline void apply_deadzone(float x, float y, float dz, float& ox, float& oy) {
    const float mag = std::sqrt(x * x + y * y);
    if (mag <= dz || mag <= 0.0f) { ox = 0.0f; oy = 0.0f; return; }
    const float scaled = (mag - dz) / (1.0f - dz) / mag;  // rescale magnitude
    ox = x * scaled;
    oy = y * scaled;
}

// Map a gamepad state to one tick's InputCommand. `look_scale` converts a unit
// right-stick deflection to look-delta radians for this tick (sensitivity).
inline contracts::InputCommand gamepad_to_input(const GamepadState& g, float look_scale) {
    contracts::InputCommand in{};
    if (!g.connected) return in;
    float mx, my;
    apply_deadzone(g.lx, g.ly, 0.20f, mx, my);
    in.move_x = mx;
    in.move_z = my;  // +up on the stick = forward (+Z)
    float rxx, ryy;
    apply_deadzone(g.rx, g.ry, 0.20f, rxx, ryy);
    in.look_yaw = rxx * look_scale;
    in.look_pitch = ryy * look_scale;
    if (g.a) in.buttons |= contracts::kButtonJump;
    if (g.run) in.buttons |= contracts::kButtonRun;
    return in;
}

}  // namespace br::app
