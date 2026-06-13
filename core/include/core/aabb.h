#pragma once
//
// core/aabb.h — axis-aligned bounding box and overlap test used by the
// capsule-vs-AABB collision solver. Deterministic, header-only.
//
#include "core/math.h"

namespace br::core {

struct Aabb {
    Vec3 mn;  // min corner
    Vec3 mx;  // max corner
};

// Strict overlap on all three axes (touching faces do not count as overlap).
inline bool overlaps(const Aabb& a, const Aabb& b) noexcept {
    return a.mn.x < b.mx.x && a.mx.x > b.mn.x &&
           a.mn.y < b.mx.y && a.mx.y > b.mn.y &&
           a.mn.z < b.mx.z && a.mx.z > b.mn.z;
}

// Build the wanderer's AABB proxy: a box of half-extents `he` centered at `c`.
inline Aabb box_at(Vec3 c, Vec3 he) noexcept {
    return Aabb{ {c.x - he.x, c.y - he.y, c.z - he.z},
                {c.x + he.x, c.y + he.y, c.z + he.z} };
}

}  // namespace br::core
