#pragma once
//
// app/flares.h — presentation-only green "chemlight" breadcrumbs (PURE, header-only).
//
// The player drops flares with R: emissive green markers that LIGHT the ray-traced scene (analytic point
// lights in the PT shader — option A, no scene geometry) and double as breadcrumbs. A fixed-capacity RING:
// past kCap the oldest recycles, so a long trail decays behind you — bounded memory + cost, and thematically
// eerie (the Backrooms "swallows" your path). NOT sim state: never hashed, never in replay, exactly like the
// flashlight — a live presentation layer only (INV-1 untouched). Pure + header-only so it is trivially testable
// and carries no graphics/sim coupling.
//
#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/math.h"

namespace br::app {

struct FlareField {
    static constexpr size_t kCap = 256;   // ring capacity; oldest recycles past this (bump freely — see pack_nearest)
    std::vector<br::core::Vec3> pts;       // active flares
    size_t writ = 0;                       // ring write cursor (once full)

    void drop(const br::core::Vec3& p) {
        if (pts.size() < kCap) pts.push_back(p);
        else { pts[writ] = p; writ = (writ + 1u) % kCap; }
    }
    void clear() { pts.clear(); writ = 0; }
    size_t count() const { return pts.size(); }

    // Pack the nearest `maxN` flares to `eye` as float4 {x,y,z,intensity} into out[4*maxN] (caller-sized).
    // Returns how many were written. This bounds the GPU's per-frame flare work to maxN regardless of how
    // many flares exist in total (a flare far from the camera neither lights nor is seen).
    uint32_t pack_nearest(const br::core::Vec3& eye, float intensity, uint32_t maxN, float* out) const {
        const size_t n = pts.size();
        if (n == 0 || maxN == 0) return 0;
        uint32_t idx[kCap];
        for (size_t i = 0; i < n; ++i) idx[i] = static_cast<uint32_t>(i);
        auto d2 = [&](uint32_t i) {
            const br::core::Vec3& p = pts[i];
            const float dx = p.x - eye.x, dy = p.y - eye.y, dz = p.z - eye.z;
            return dx * dx + dy * dy + dz * dz;
        };
        const size_t take = (n < maxN) ? n : maxN;
        std::partial_sort(idx, idx + take, idx + n, [&](uint32_t a, uint32_t b) { return d2(a) < d2(b); });
        for (size_t i = 0; i < take; ++i) {
            const br::core::Vec3& p = pts[idx[i]];
            out[4 * i + 0] = p.x; out[4 * i + 1] = p.y; out[4 * i + 2] = p.z; out[4 * i + 3] = intensity;
        }
        return static_cast<uint32_t>(take);
    }
};

}  // namespace br::app
