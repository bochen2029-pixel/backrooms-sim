#pragma once
//
// app/ladder.h — "the infinite ladder": ONE continuous 45-degree staircase that descends along +X (1 m down
// per 1 m forward) and ascends along -X, forever — traverse every floor without stopping; step off sideways
// onto any floor. Pure, header-only, APP-LAYER ONLY (gated walk/soak/replay paths build the world without it,
// so zero determinism/golden impact).
//
// Hard-won lessons (a full-codebase fan-out diagnosed the first attempt; E37 fixed the visual carve):
//   * COLLISION step-slabs must be DEEP (span a level), not thin — else when the per-level flat floor is
//     rebuilt under you on a level crossing (level_from_y flips), a thin slab leaves a seam and you fall
//     through. Deep, X-contiguous slabs guarantee solid ground at every tick. Riser 0.5 m < core::kStepHeight
//     (0.55) so the capsule's step-up mounts each riser BOTH ways (exactly like the M27 stairs).
//   * The RENDER mesh must (a) stay UNDER the renderer's per-chunk slot cap (6144 verts) — so SINGLE winding
//     (cull is NONE anyway) + a bounded vertex count, and (b) use material kMatFluorescent (3.0), whose lit
//     shader branch is EMISSIVE — so the steps glow and read as distinct, not blend into the yellow walls.
//   * EMISSIVE means the lighting system contributes NOTHING — depth cues must be BAKED into the vertex
//     colors (per-face shade multipliers: bright treads, dimmer risers, dark sides/underside), or up close
//     the whole staircase reads as one flat neon cutout (the E37 "looks horrible" report).
//   * Carve WHOLE boxes, never single faces. gen emits walls/pillars as one-sided BOXES; every mesh face is
//     one-sided (backfaces render near-black under the lit shader). The old centroid-per-quad carve dropped
//     only the band-facing face of the lane's edge walls -> the lane was lined with black box interiors, and
//     crossing walls left floating top ribbons. The z-interval-overlap rule below drops all of a box's faces
//     together — and it MATCHES apply_to_collision, which already drops those same whole boxes (the lane is
//     an open gallery by design: you step off the ladder onto any floor).
//   * The SPUR must carve render AND collision the same way — the old render-only wall drop left INVISIBLE
//     solid walls on the spawn approach for seeds whose maze put a wall there.
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
constexpr float kSkirtVis = 1.6f;   // RENDER body depth below each tread: a solid diagonal beam (escalator body) that
                                    // also plugs the 1 m inter-floor slab void where the run punches through a plane
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
// band clear (drop every floor/wall AABB overlapping the Z-band so the ramp shaft is open), carve the spur's
// WALLS (tall boxes only — floors stay; mirrors carve_band_mesh so nothing is ever visible-but-hollow or
// solid-but-invisible), then drop in the diagonal DEEP step run. The deep slabs ARE the floor inside the band.
inline void apply_to_collision(std::vector<br::core::Aabb>& c, contracts::ChunkKey key) {
    const float zlo = kAnchorZ - kHalfW, zhi = kAnchorZ + kHalfW;
    const float sx0 = kAnchorX - kSpurHalfW, sx1 = kAnchorX + kSpurHalfW;
    c.erase(std::remove_if(c.begin(), c.end(), [&](const br::core::Aabb& a) {
                const bool band = a.mn.z < zhi && a.mx.z > zlo;   // overlaps the band in Z -> carve (clear the shaft)
                const bool spurWall = a.mx.z > kSpurZ0 && a.mn.z < zlo && a.mx.x > sx0 && a.mn.x < sx1 &&
                                      (a.mx.y - a.mn.y) > 1.0f;   // spur: tall boxes (walls/pillars) only, floors stay
                return band || spurWall;
            }), c.end());
    const float x0 = static_cast<float>(key.cx - 1) * contracts::kChunkSize;
    const float x1 = static_cast<float>(key.cx + 2) * contracts::kChunkSize;
    steps_in_x(c, x0, x1);
}

// ----- render mesh (raster), EMISSIVE so it glows + reads distinct -----------
namespace detail {
// `shade` bakes the depth cue INTO the emissive color (the lit shader's fluorescent branch ignores lighting):
// bright treads / dimmer risers / dark sides+underside make the steps read as 3-D instead of a flat cutout.
inline void face(std::vector<contracts::ChunkVertex>& o, br::core::Vec3 a, br::core::Vec3 b,
                 br::core::Vec3 c, br::core::Vec3 d, float nx, float ny, float nz, float shade) {
    const br::core::Vec3 q[6] = { a, b, c, a, c, d };   // single winding (lit PSO CullMode = NONE)
    for (int i = 0; i < 6; ++i) {
        contracts::ChunkVertex v{};
        v.pos[0] = q[i].x; v.pos[1] = q[i].y; v.pos[2] = q[i].z;
        v.nrm[0] = nx; v.nrm[1] = ny; v.nrm[2] = nz;
        v.color[0] = 0.20f * shade; v.color[1] = 0.95f * shade; v.color[2] = 1.00f * shade;   // cyan × baked face shade
        v.uv[0] = 0.5f; v.uv[1] = 0.5f;
        v.material = contracts::kMatFluorescent;   // 3.0 -> emissive branch in the lit shader -> the steps GLOW
        o.push_back(v);
    }
}
}  // namespace detail

// Build the visible ladder as a chunk-vertex mesh (injected as a synthetic ResidentChunk): a SOLID diagonal
// beam of steps — tread + riser lip + deep side skirts + underside — so it reads as an escalator body, hides
// the one-sided world backfaces behind it, and plugs the inter-floor slab void at every punch-through. Only
// steps within `reach` of camX (X≡Y on the ramp bounds the vertical span too). Budget: 6 faces × 6 verts =
// 36 verts/step; reach 38 -> ~155 steps ≈ 5580 verts, under the renderer's 6144-vertex slot cap.
inline void build_mesh(std::vector<contracts::ChunkVertex>& out, float camX, float reach) {
    using V = br::core::Vec3;
    out.clear();
    const float z0 = kAnchorZ - kHalfW, z1 = kAnchorZ + kHalfW;
    const int i0 = static_cast<int>(std::floor((camX - reach - kAnchorX) / kStep));
    const int i1 = static_cast<int>(std::ceil((camX + reach - kAnchorX) / kStep));
    for (int i = i0; i <= i1; ++i) {
        const float xa = kAnchorX + static_cast<float>(i) * kStep, xb = xa + kStep;
        const float yt = -static_cast<float>(i) * kStep, yb = yt - kSkirtVis;   // deep body (adjacent boxes overlap -> solid)
        const float tread = ((i & 1) == 0) ? 1.00f : 0.84f;   // alternate treads so each step reads under your feet
        detail::face(out, V{xa,yt,z0}, V{xa,yt,z1}, V{xb,yt,z1}, V{xb,yt,z0}, 0, 1, 0, tread);       // top tread
        detail::face(out, V{xa,yb,z0}, V{xa,yt,z0}, V{xa,yt,z1}, V{xa,yb,z1}, -1, 0, 0, 0.55f);      // front riser (-X)
        detail::face(out, V{xb,yb,z1}, V{xb,yt,z1}, V{xb,yt,z0}, V{xb,yb,z0}, 1, 0, 0, 0.45f);       // back (+X)
        detail::face(out, V{xa,yb,z0}, V{xb,yb,z0}, V{xb,yt,z0}, V{xa,yt,z0}, 0, 0, -1, 0.34f);      // skirt (-Z)
        detail::face(out, V{xb,yb,z1}, V{xa,yb,z1}, V{xa,yt,z1}, V{xb,yt,z1}, 0, 0, 1, 0.34f);       // skirt (+Z)
        detail::face(out, V{xa,yb,z1}, V{xa,yb,z0}, V{xb,yb,z0}, V{xb,yb,z1}, 0, -1, 0, 0.20f);      // underside
    }
}

// ----- render carve: open the stairwell lane + punch a hole where the diagonal crosses each floor ---------------
// The generator's per-cell floor/wall/ceiling still renders inside the band, occluding the steps. But fully
// CLEARING the band opens an infinite vertical trench: the engine keeps only the current floor + ONE neighbour
// resident, so you'd see ~2 levels then void, the creature several floors up reads through (x-ray), and the cell
// edges rip. Instead, the M27 stairwell pattern: keep every floor/ceiling INTACT and remove only a small hole
// where the 45-deg surface actually punches through that horizontal plane's height (surface_y(x)==y  =>
// x == kAnchorX - y). Intact floors occlude => no trench, no void, no x-ray.
//
// E37 carve rule — WHOLE boxes by z-interval overlap, never per-face centroids. gen's walls/pillars/steps are
// one-sided BOXES; the old centroid test dropped only the face whose centre fell inside the band, so the lane's
// edge walls kept their outer face (seen from the lane: a black box interior), crossing walls left floating top
// ribbons, and the spur left half-carved boxes. Now a box's faces all share its z-extent, so one interval test
// drops or keeps them TOGETHER — which also exactly matches apply_to_collision's whole-AABB carve (the lane is
// an open gallery: its edge walls are non-solid by design, so they must be invisible too, not black).
// Returns true if anything was dropped (only the band Z-row, cz==0, drops). `out` is caller-owned scratch.
inline bool carve_band_mesh(const contracts::ChunkVertex* v, uint32_t count,
                            std::vector<contracts::ChunkVertex>& out) {
    out.clear();
    bool dropped = false;
    constexpr float kEps = 0.01f;   // open-interval overlap: a box merely ABUTTING the band/spur edge is kept
    const float zlo = kAnchorZ - kHalfW, zhi = kAnchorZ + kHalfW;
    const float sx0 = kAnchorX - kSpurHalfW, sx1 = kAnchorX + kSpurHalfW;
    // The world mesh is a flat list of QUADS (push_quad -> 6 contiguous verts each). Decide per-QUAD, never per
    // triangle: dropping one of a quad's two triangles leaves a diagonal-cut "half rectangle" floating where it
    // shouldn't be. Extent-test the whole quad, then drop or keep all 6 verts together.
    uint32_t q = 0;
    for (; q + 6 <= count; q += 6) {
        float xc = 0.0f, yc = 0.0f, zc = 0.0f;
        float xmn = v[q].pos[0], xmx = xmn, zmn = v[q].pos[2], zmx = zmn;
        for (uint32_t k = 0; k < 6; ++k) {
            const float px = v[q + k].pos[0], pz = v[q + k].pos[2];
            xc += px; yc += v[q + k].pos[1]; zc += pz;
            xmn = (px < xmn) ? px : xmn; xmx = (px > xmx) ? px : xmx;
            zmn = (pz < zmn) ? pz : zmn; zmx = (pz > zmx) ? pz : zmx;
        }
        xc *= (1.0f / 6.0f); yc *= (1.0f / 6.0f); zc *= (1.0f / 6.0f);
        const float ny = v[q].nrm[1];
        const bool horizontal = (ny > 0.5f || ny < -0.5f);   // floor/ceiling cell vs box face
        const bool bandOverlap = (zmx > zlo + kEps) && (zmn < zhi - kEps);
        const bool spurOverlap = (zmx > kSpurZ0 + kEps) && (zmn < zlo - kEps) &&
                                 (xmx > sx0 + kEps) && (xmn < sx1 - kEps);   // spur: walls/pillars go, floors stay
        bool drop = false;
        if (!horizontal) {
            drop = bandOverlap || spurOverlap;               // any box face overlapping the lane/spur -> whole box goes
        } else if ((xmx - xmn) <= 1.05f || (zmx - zmn) <= 1.05f) {
            drop = bandOverlap || spurOverlap;               // thin horizontal = a box TOP (wall/pillar/step) -> goes with its box
        } else if (zc > zlo && zc < zhi) {                   // real floor/ceiling cell in the band: punch-through hole only
            const float xStair = kAnchorX - yc;              // where the diagonal crosses this plane's height
            if (xc > xStair - kHoleHalfW && xc < xStair + kHoleHalfW)
                drop = true;
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
