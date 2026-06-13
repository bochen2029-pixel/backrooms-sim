#pragma once
//
// core/math.h — minimal deterministic vector math for the sim core.
//
// Float math only; the core compiles /fp:strict (ADR-003) and runs
// single-threaded, so results are bit-identical run to run (INV-1). No SIMD
// intrinsics, no fast-math. Transcendental use (sin/cos in camera basis) is
// deterministic for a fixed binary/CRT.
//
#include <cmath>

namespace br::core {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }

inline float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

inline float length(Vec3 a) noexcept { return std::sqrt(dot(a, a)); }

inline Vec3 normalize(Vec3 a) noexcept {
    const float len = length(a);
    return len > 0.0f ? a * (1.0f / len) : Vec3{};
}

inline float clampf(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace br::core
