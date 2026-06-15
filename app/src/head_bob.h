#pragma once
//
// app/head_bob.h — humanlike camera head-bob (M18), header-only and PURE.
//
// A view-layer effect: the bob is a pure function of the *walked distance* (the
// deterministic sim odometer), the current horizontal speed, and the walk/run
// reference speeds. It is applied to the RENDER camera only — never to WorldState —
// so determinism (INV-1) and every golden are untouched, yet the bob is perfectly
// reproducible (it's driven by the deterministic odometer).
//
// Rhythm (standard FPS practice): the head dips on each footfall — two dips per
// stride cycle (vertical) — while it sways side to side once per cycle (lateral),
// at half the vertical frequency, tracing a figure-8 / Lissajous. Running raises
// both the cadence and the amplitude. The amplitude eases in with speed (smoothstep)
// so coming to a stop fades the bob out instead of snapping it.
//
#include <cmath>

namespace br::app {

struct BobOffset {
    float dy = 0.0f;  // vertical camera offset (m), <= 0 (the head dips)
    float dx = 0.0f;  // lateral offset along the camera's right vector (m)
};

inline BobOffset head_bob(float distance_m, float speed, float walk_speed, float run_speed) {
    constexpr float kTwoPi = 6.28318530718f;
    const bool running = speed > (walk_speed + run_speed) * 0.5f;

    // Ease the bob in/out with speed (smoothstep over [0.1, 0.6*walk]); a stop -> 0.
    const float lo = 0.1f, hi = walk_speed * 0.6f;
    float t = (hi > lo) ? (speed - lo) / (hi - lo) : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float ramp = t * t * (3.0f - 2.0f * t);

    const float cycles_per_m = running ? 0.42f : 0.31f;  // stride cadence (cycles / metre)
    const float amp_v = (running ? 0.085f : 0.050f) * ramp;
    const float amp_l = (running ? 0.050f : 0.030f) * ramp;
    const float phase = distance_m * cycles_per_m * kTwoPi;

    BobOffset o;
    o.dy = -amp_v * 0.5f * (1.0f - std::cos(2.0f * phase));  // two dips / cycle (footfalls), always <= 0
    o.dx = amp_l * std::sin(phase);                           // figure-8 sway, half the vertical frequency
    return o;
}

}  // namespace br::app
