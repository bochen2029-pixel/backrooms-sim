#pragma once
//
// app/ladder.h — "the infinite ladder": ONE continuous 45-degree staircase that descends along +X (1 m down
// per 1 m forward) and ascends along -X, forever — traverse every floor without stopping; step off sideways
// onto any floor. Pure, header-only, APP-LAYER ONLY (gated walk/soak/replay paths build the world without it,
// so zero determinism/golden impact).
//
// Hard-won lessons (a full-codebase fan-out diagnosed the first attempt):
//   * COLLISION step-slabs must be DEEP (span a level), not thin — else when the per-level flat floor is
//     rebuilt under you on a level crossing (level_from_y flips), a thin slab leaves a seam and you fall
//     through. Deep, X-contiguous slabs guarantee solid ground at every tick. Riser 0.5 m < core::kStepHeight
//     (0.55) so the capsule's step-up mounts each riser BOTH ways (exactly like the M27 stairs).
//   * The RENDER mesh must (a) stay UNDER the renderer's per-chunk slot cap (6144 verts) — so SINGLE winding
//     (cull is NONE anyway) + a bounded vertex count, and (b) use material kMatFluorescent (3.0), whose lit
//     shader branch is EMISSIVE — so the steps glow and read as distinct, not blend into the yellow walls.
//   * Placement: anchored a short walk +Z of the spawn cell (NOT under spawn), at floor level where you reach it.
//
#include <algorithm>
#include <cmath>
#include <vector>

#include "contracts/chunk_gen_v1.h"
#include "core/world.h"

namespace br::app {
namespace ladder {

constexpr float kAnchorX = 2.0f;    // ramp crosses level-0 floor (Y=0) at this X (== spawn X / a 4 m cell centre, so holes land on whole cells)
constexpr float kAnchorZ = 10.0f;   // band centreline Z = cell j=2 centre -> band edges Z=8,12 fall on CELL BOUNDARIES (never split a quad)
constexpr float kHalfW   = 2.0f;    // band half-width -> Z in [8,12] == exactly one 4 m cell row. Spawn cell Z[0,4] stays solid.
constexpr float kStep    = 0.5f;    // run == rise -> 45 deg; riser 0.5 < core::kStepHeight 0.55 -> walkable up & down
constexpr float kDeep    = 6.0f;    // COLLISION slab depth (>= 1.5 levels) -> no fall-through across a level rebuild
constexpr float kRiserVis = 0.5f;   // RENDER riser/side height (just the visible step face, not the deep collision body)
constexpr float kSpurZ0  = 1.0f;    // a clear walk-up from the spawn cell (Z~2) into the band, so you can reach the ladder
constexpr float kSpurHalfW = 3.0f;  // spur half-width in X about the crossing (kAnchorX) -> X in [-1,5]
constexpr float kHoleHalfW = 2.5f;  // floor/ceiling HOLE half-width (X): drops the whole 4 m cell the diagonal punches through

// The continuous 45-degree surface Y at world X (descends as X grows; anchorY = 0).
inline float surface_y(float x) { return -(x - kAnchorX); }

inline bool z_in_band(float z) { return z >= kAnchorZ - kHalfW && z <= kAnchorZ + kHalfW; }

// Append the DEEP step-slab AABBs covering world-X [x0,x1] (each: top at surface_y(Xi), kDeep tall, band-wide).
inline void steps_in_x(std::vector<br::core::Aabb>& out, float x0, float x1) {
    const int i0 = static_cast<int>(std::floor((x0 - kAnchorX) / kStep)) - 1;
    const int i1 = static_cast<int>(std::ceil((x1 - kAnchorX) / kStep)) + 1;
    for (int i = i0; i <= i1; ++i) {
        const float xa = kAnchorX + static_cast<float>(i) * kStep;
        const float yt = -static_cast<float>(i) * kStep;   // == surface_y(xa)
        out.push_back(br::core::Aabb{ {xa, yt - kDeep, kAnchorZ - kHalfW}, {xa + kStep, yt, kAnchorZ + kHalfW} });
    }
}

// Apply the ladder to an already-built interactive collision set for the 3x3 chunks around `key`: carve the
// band clear (drop every floor/wall AABB overlapping the Z-band so the ramp shaft is open), then drop in the
// diagonal DEEP step run. The deep slabs ARE the floor inside the band at every level.
inline void apply_to_collision(std::vector<br::core::Aabb>& c, contracts::ChunkKey key) {
    const float zlo = kAnchorZ - kHalfW, zhi = kAnchorZ + kHalfW;
    c.erase(std::remove_if(c.begin(), c.end(), [&](const br::core::Aabb& a) {
                return a.mn.z < zhi && a.mx.z > zlo;   // overlaps the band in Z -> carve (clear the shaft)
            }), c.end());
    const float x0 = static_cast<float>(key.cx - 1) * contracts::kChunkSize;
    const float x1 = static_cast<float>(key.cx + 2) * contracts::kChunkSize;
    steps_in_x(c, x0, x1);
}

// ----- render mesh (raster), EMISSIVE so it glows + reads distinct -----------
namespace detail {
inline void face(std::vector<contracts::ChunkVertex>& o, br::core::Vec3 a, br::core::Vec3 b,
                 br::core::Vec3 c, br::core::Vec3 d, float nx, float ny, float nz) {
    const br::core::Vec3 q[6] = { a, b, c, a, c, d };   // single winding (lit PSO CullMode = NONE)
    for (int i = 0; i < 6; ++i) {
        contracts::ChunkVertex v{};
        v.pos[0] = q[i].x; v.pos[1] = q[i].y; v.pos[2] = q[i].z;
        v.nrm[0] = nx; v.nrm[1] = ny; v.nrm[2] = nz;
        v.color[0] = 0.20f; v.color[1] = 0.95f; v.color[2] = 1.00f;   // cyan hue tint (× the bright fluorescent slice)
        v.uv[0] = 0.5f; v.uv[1] = 0.5f;
        v.material = contracts::kMatFluorescent;   // 3.0 -> emissive branch in the lit shader -> the steps GLOW
        o.push_back(v);
    }
}
}  // namespace detail

// Build the visible ladder steps as a chunk-vertex mesh (injected as a synthetic ResidentChunk). Only steps
// within `reach` of camX (which, since X≡Y on the ramp, also bounds the vertical span). Keep `reach` modest so
// the mesh stays under the renderer's 6144-vertex slot cap: ~30 verts/step -> reach 34 ≈ 4080 verts.
inline void build_mesh(std::vector<contracts::ChunkVertex>& out, float camX, float reach) {
    using V = br::core::Vec3;
    out.clear();
    const float z0 = kAnchorZ - kHalfW, z1 = kAnchorZ + kHalfW;
    const int i0 = static_cast<int>(std::floor((camX - reach - kAnchorX) / kStep));
    const int i1 = static_cast<int>(std::ceil((camX + reach - kAnchorX) / kStep));
    for (int i = i0; i <= i1; ++i) {
        const float xa = kAnchorX + static_cast<float>(i) * kStep, xb = xa + kStep;
        const float yt = -static_cast<float>(i) * kStep, yb = yt - kRiserVis;   // visible step face only
        detail::face(out, V{xa,yt,z0}, V{xa,yt,z1}, V{xb,yt,z1}, V{xb,yt,z0}, 0, 1, 0);     // top tread
        detail::face(out, V{xa,yb,z0}, V{xa,yt,z0}, V{xa,yt,z1}, V{xa,yb,z1}, -1, 0, 0);    // front riser (-X)
        detail::face(out, V{xb,yb,z1}, V{xb,yt,z1}, V{xb,yt,z0}, V{xb,yb,z0}, 1, 0, 0);     // back (+X)
        detail::face(out, V{xa,yb,z0}, V{xb,yb,z0}, V{xb,yt,z0}, V{xa,yt,z0}, 0, 0, -1);    // side (-Z)
        detail::face(out, V{xb,yb,z1}, V{xa,yb,z1}, V{xa,yt,z1}, V{xb,yt,z1}, 0, 0, 1);     // side (+Z)
    }
}

// ----- render carve: open the stairwell lane + punch a hole where the diagonal crosses each floor ---------------
// The generator's per-cell floor/wall/ceiling still renders inside the band, occluding the steps. But fully
// CLEARING the band opens an infinite vertical trench: the engine keeps only the current floor + ONE neighbour
// resident, so you'd see ~2 levels then void, the creature several floors up reads through (x-ray), and the cell
// edges rip. Instead, the M27 stairwell pattern: keep every floor/ceiling INTACT and remove only a small hole
// where the 45-deg surface actually punches through that horizontal plane's height (surface_y(x)==y  =>
// x == kAnchorX - y). Intact floors occlude => no trench, no void, no x-ray. Walls in the lane are still cleared
// (the stair runs the whole length). The short spawn spur drops walls only (keeps its floor). Returns true if
// anything was dropped (only the band Z-row, cz==0, drops). `out` is caller-owned persistent scratch.
inline bool carve_band_mesh(const contracts::ChunkVertex* v, uint32_t count,
                            std::vector<contracts::ChunkVertex>& out) {
    out.clear();
    bool dropped = false;
    // The world mesh is a flat list of QUADS (push_quad -> 6 contiguous verts each). Decide per-QUAD, never per
    // triangle: dropping one of a quad's two triangles leaves a diagonal-cut "half rectangle" floating where it
    // shouldn't be. Test the whole quad's centroid + normal, then drop or keep all 6 verts together.
    uint32_t q = 0;
    for (; q + 6 <= count; q += 6) {
        float xc = 0.0f, yc = 0.0f, zc = 0.0f;
        for (uint32_t k = 0; k < 6; ++k) { xc += v[q + k].pos[0]; yc += v[q + k].pos[1]; zc += v[q + k].pos[2]; }
        xc *= (1.0f / 6.0f); yc *= (1.0f / 6.0f); zc *= (1.0f / 6.0f);
        const float ny = v[q].nrm[1];
        const bool horizontal = (ny > 0.5f || ny < -0.5f);   // floor/ceiling vs wall
        bool drop = false;
        if (zc > kAnchorZ - kHalfW && zc < kAnchorZ + kHalfW) {
            if (!horizontal) {
                drop = true;                                  // BAND wall -> clear the stairwell lane (whole length)
            } else {
                const float xStair = kAnchorX - yc;           // where the diagonal crosses this plane's height
                if (xc > xStair - kHoleHalfW && xc < xStair + kHoleHalfW)
                    drop = true;                              // BAND floor/ceiling -> hole only at the punch-through cell
            }
        } else if (zc > kSpurZ0 && zc <= kAnchorZ - kHalfW &&
                   xc > kAnchorX - kSpurHalfW && xc < kAnchorX + kSpurHalfW) {
            if (!horizontal) drop = true;                     // SPUR: drop walls only, keep floor/ceiling
        }
        if (drop) { dropped = true; continue; }
        for (uint32_t k = 0; k < 6; ++k) out.push_back(v[q + k]);
    }
    for (; q < count; ++q) out.push_back(v[q]);               // tail (non-quad remainder, if any) -> pass through intact
    return dropped;
}

// Build the band-carved resident set for one frame: cz==0 chunks are replaced by carved copies (into the
// persistent `pool`, resized once up-front so the pointers stay valid for the frame), all others pass through.
inline void carve_residents(const std::vector<contracts::ResidentChunk>& in,
                            std::vector<std::vector<contracts::ChunkVertex>>& pool,
                            std::vector<contracts::ResidentChunk>& out) {
    size_t nCarve = 0;
    for (const auto& rc : in) if (rc.key.cz == 0) ++nCarve;
    if (pool.size() < nCarve) pool.resize(nCarve);
    out.clear();
    out.reserve(in.size() + 4);
    size_t ci = 0;
    for (const auto& rc : in) {
        if (rc.key.cz == 0 && carve_band_mesh(rc.vertices, rc.vertex_count, pool[ci])) {
            out.push_back(contracts::ResidentChunk{ rc.key, pool[ci].data(),
                                                    static_cast<uint32_t>(pool[ci].size()) });
            ++ci;
        } else {
            out.push_back(rc);
        }
    }
}

}  // namespace ladder
}  // namespace br::app
