#pragma once
//
// app/shoggoth_body.h — the Shoggoth's procedural body mesh (M20b), header-only & pure.
//
// A warm salmon/orange radial burst of tapered, writhing tentacle-spokes around a
// core (operator reference). No assets — generated each frame in WORLD space at the
// creature's position, in the chunk vertex format, so it draws through the existing
// lit pipeline (injected as a synthetic ResidentChunk). The writhe phase animates the
// tentacles (pulsing length + a curving wave) so it never looks static.
//
#include <cmath>
#include <cstdint>
#include <vector>

#include "contracts/chunk_gen_v1.h"
#include "core/world.h"

namespace br::app {

namespace detail {
inline br::core::Vec3 vadd(const br::core::Vec3& a, const br::core::Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline br::core::Vec3 vmul(const br::core::Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline br::core::Vec3 vcross(const br::core::Vec3& a, const br::core::Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline br::core::Vec3 vnorm(const br::core::Vec3& a) {
    const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return l > 1e-6f ? br::core::Vec3{a.x / l, a.y / l, a.z / l} : br::core::Vec3{0, 1, 0};
}
}  // namespace detail

// Append a triangle (warm-orange, brightness `b`) with a computed face normal.
inline void shog_tri(std::vector<contracts::ChunkVertex>& out, const br::core::Vec3& a,
                     const br::core::Vec3& b, const br::core::Vec3& c, float bright) {
    using namespace detail;
    const br::core::Vec3 n = vnorm(vcross({b.x - a.x, b.y - a.y, b.z - a.z}, {c.x - a.x, c.y - a.y, c.z - a.z}));
    const float r = 0.92f * bright, g = 0.42f * bright, bl = 0.34f * bright;  // salmon/orange
    const br::core::Vec3 tri[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        contracts::ChunkVertex v{};
        v.pos[0] = tri[i].x; v.pos[1] = tri[i].y; v.pos[2] = tri[i].z;
        v.nrm[0] = n.x; v.nrm[1] = n.y; v.nrm[2] = n.z;
        v.color[0] = r; v.color[1] = g; v.color[2] = bl;
        v.uv[0] = 0.5f; v.uv[1] = 0.5f; v.material = 0.0f;
        out.push_back(v);
    }
}

inline void build_shoggoth_mesh(std::vector<contracts::ChunkVertex>& out,
                                const br::core::Vec3& pos, float writhe, float scale) {
    using namespace detail;
    out.clear();
    const int T = 13;                  // tentacle count
    const float core = 0.30f * scale;  // central mass radius
    const int SEG = 4, RAD = 4;        // segments per tentacle, cross-section verts

    for (int i = 0; i < T; ++i) {
        const float t = (i + 0.5f) / T;
        const float phi = std::acos(1.0f - 2.0f * t);  // inclination 0..pi
        const float theta = 2.39996323f * i;           // golden angle
        const br::core::Vec3 dir{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
        const float w = writhe + i * 0.7f;
        const float len = (1.0f + 0.4f * std::sin(w * 1.3f)) * scale;  // pulsing reach

        // local frame perpendicular to dir
        br::core::Vec3 up{0, 1, 0};
        if (std::fabs(dir.y) > 0.9f) up = {1, 0, 0};
        const br::core::Vec3 side = vnorm(vcross(dir, up));
        const br::core::Vec3 nrm2 = vnorm(vcross(dir, side));

        auto ring_center = [&](float ft) {
            const float curve = 0.28f * len * std::sin(w + ft * 3.0f) * ft;  // grows toward the tip
            return vadd(pos, vadd(vmul(dir, core + len * ft), vmul(side, curve)));
        };
        auto ring_radius = [&](float ft) { return core * 0.7f * (1.0f - ft) + 0.02f; };

        for (int s = 0; s < SEG; ++s) {
            const float ft0 = static_cast<float>(s) / SEG, ft1 = static_cast<float>(s + 1) / SEG;
            const br::core::Vec3 c0 = ring_center(ft0), c1 = ring_center(ft1);
            const float r0 = ring_radius(ft0), r1 = ring_radius(ft1);
            const float b0 = 1.05f - 0.5f * ft0, b1 = 1.05f - 0.5f * ft1;  // brighter at the core
            for (int k = 0; k < RAD; ++k) {
                const float a0 = (k / static_cast<float>(RAD)) * 6.2831853f;
                const float a1 = ((k + 1) / static_cast<float>(RAD)) * 6.2831853f;
                auto on = [&](const br::core::Vec3& ctr, float rad, float ang) {
                    return vadd(ctr, vadd(vmul(side, std::cos(ang) * rad), vmul(nrm2, std::sin(ang) * rad)));
                };
                const br::core::Vec3 p00 = on(c0, r0, a0), p01 = on(c0, r0, a1);
                const br::core::Vec3 p10 = on(c1, r1, a0), p11 = on(c1, r1, a1);
                shog_tri(out, p00, p10, p11, 0.5f * (b0 + b1));
                shog_tri(out, p00, p11, p01, 0.5f * (b0 + b1));
            }
        }
    }
}

}  // namespace br::app
