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
// E38 "infinite" visualization: stepped geometry only where steps are resolvable; beyond that the beam continues
// as a smooth 45-deg prism whose baked brightness FADES to black with distance -- it visibly vanishes into the
// dark above and below (no pop-out), reading as an endless run. All camX-relative; the mesh rebuilds on an 8 m
// key so the fade window follows the wanderer.
constexpr float kNearReach   = 18.0f;    // stepped zone half-length (steps read individually out to here)
constexpr float kFarReach    = 220.0f;   // beam fade-out half-length (brightness reaches 0 -> "infinite" vanish)
constexpr int   kFarSegs     = 12;       // prism segments per direction (piecewise-linear fade gradient)
constexpr float kCollarReach = 30.0f;    // line the punch-through slab voids within this range of the wanderer
// Warm palette (E38): the old chemlight cyan fought the Backrooms' yellow; the body now glows warm amber-gold
// (the lit shader half-desaturates vertex tints, so the baked hue is pushed warmer than the target on purpose).
constexpr float kBodyR = 1.00f, kBodyG = 0.87f, kBodyB = 0.52f;
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
// Baked shading: the lit shader's fluorescent branch ignores lighting, so every depth cue lives in the vertex
// color (warm body tint × per-face shade). `face_grad` shades the a/b edge with s0 and the c/d edge with s1 —
// the rasterizer interpolates between them, which is how the far beam gets its smooth fade-to-black gradient.
inline void face_grad(std::vector<contracts::ChunkVertex>& o, br::core::Vec3 a, br::core::Vec3 b,
                      br::core::Vec3 c, br::core::Vec3 d, float nx, float ny, float nz, float s0, float s1) {
    const br::core::Vec3 q[6] = { a, b, c, a, c, d };            // single winding (lit PSO CullMode = NONE)
    const float s[6] = { s0, s0, s1, s0, s1, s1 };               // a,b carry s0; c,d carry s1
    for (int i = 0; i < 6; ++i) {
        contracts::ChunkVertex v{};
        v.pos[0] = q[i].x; v.pos[1] = q[i].y; v.pos[2] = q[i].z;
        v.nrm[0] = nx; v.nrm[1] = ny; v.nrm[2] = nz;
        v.color[0] = kBodyR * s[i]; v.color[1] = kBodyG * s[i]; v.color[2] = kBodyB * s[i];
        v.uv[0] = 0.5f; v.uv[1] = 0.5f;
        v.material = contracts::kMatFluorescent;   // 3.0 -> emissive branch in the lit shader -> the body GLOWS
        o.push_back(v);
    }
}
inline void face(std::vector<contracts::ChunkVertex>& o, br::core::Vec3 a, br::core::Vec3 b,
                 br::core::Vec3 c, br::core::Vec3 d, float nx, float ny, float nz, float shade) {
    face_grad(o, a, b, c, d, nx, ny, nz, shade, shade);
}
// The distance fade of the far beam: 1.0 at the stepped seam easing to 0.0 at kFarReach (slightly convex so the
// vanish lingers). This is what turns a finite mesh into "it disappears into infinite distance" — the end of the
// geometry is black, so there is nothing to pop.
inline float fade_at(float x, float camX) {
    const float d = (x > camX) ? (x - camX) : (camX - x);
    float t = (kFarReach - d) / (kFarReach - kNearReach);
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    return t * std::sqrt(t);   // t^1.5
}
}  // namespace detail

// Build the visible ladder as a chunk-vertex mesh (injected as a synthetic ResidentChunk).
// Three zones, one draw:
//   1. STEPPED body (|x-camX| <= kNearReach): tread + riser lip + deep skirts + underside — a solid escalator
//      beam with baked per-face shading (alternating treads so each step reads underfoot).
//   2. FAR PRISMS (out to kFarReach both ways): the same beam as a smooth 45° prism (steps are sub-pixel out
//      there) whose brightness fades to black with distance — the run visibly vanishes up and down forever.
//   3. VOID COLLARS (|x-camX| <= kCollarReach): the 1 m structural slab void the run punches through at every
//      level is lined with dark warm panels (2 side linings + top/bottom plates per crossing), so the rim reads
//      as a finished stairwell opening instead of raw black backfaces.
// Budget @ defaults: steps 73×36=2628 + prisms 2×12×24=576 + collars ~15×4×6=360 ≈ 3.6 k verts (cap 6144).
inline void build_mesh(std::vector<contracts::ChunkVertex>& out, float camX, float reach) {
    using V = br::core::Vec3;
    out.clear();
    const float z0 = kAnchorZ - kHalfW, z1 = kAnchorZ + kHalfW;
    const float nearR = (reach < kNearReach) ? reach : kNearReach;

    // 1) stepped zone, ending exactly on step-grid boundaries so the prisms continue from the seam.
    const int i0 = static_cast<int>(std::floor((camX - nearR - kAnchorX) / kStep));
    const int i1 = static_cast<int>(std::ceil((camX + nearR - kAnchorX) / kStep));
    for (int i = i0; i <= i1; ++i) {
        const float xa = kAnchorX + static_cast<float>(i) * kStep, xb = xa + kStep;
        const float yt = -static_cast<float>(i) * kStep, yb = yt - kSkirtVis;   // deep body (adjacent boxes overlap -> solid)
        const bool alt = ((i & 1) == 0);
        const float tread = alt ? 1.00f : 0.84f;   // alternate treads so each step reads under your feet
        const float rFrnt = alt ? 0.58f : 0.46f;   // risers alternate too: seen stacked from below (looking up the
        const float rBack = alt ? 0.46f : 0.35f;   // run) the banding is what makes the wall of steps read as STEPS
        detail::face(out, V{xa,yt,z0}, V{xa,yt,z1}, V{xb,yt,z1}, V{xb,yt,z0}, 0, 1, 0, tread);       // top tread
        detail::face(out, V{xa,yb,z0}, V{xa,yt,z0}, V{xa,yt,z1}, V{xa,yb,z1}, -1, 0, 0, rFrnt);      // front riser (-X)
        detail::face(out, V{xb,yb,z1}, V{xb,yt,z1}, V{xb,yt,z0}, V{xb,yb,z0}, 1, 0, 0, rBack);       // back (+X)
        detail::face(out, V{xa,yb,z0}, V{xb,yb,z0}, V{xb,yt,z0}, V{xa,yt,z0}, 0, 0, -1, 0.34f);      // skirt (-Z)
        detail::face(out, V{xb,yb,z1}, V{xa,yb,z1}, V{xa,yt,z1}, V{xb,yt,z1}, 0, 0, 1, 0.34f);       // skirt (+Z)
        detail::face(out, V{xa,yb,z1}, V{xa,yb,z0}, V{xb,yb,z0}, V{xb,yb,z1}, 0, -1, 0, 0.20f);      // underside
    }

    // 2) far prisms: a smooth beam continuing from each end of the stepped zone out to kFarReach, brightness
    //    fading to zero (top through the tread LEADING edges: y = surface_y(x), so the seam lines up).
    if (reach > nearR + kStep) {
        const float farR = (reach < kFarReach) ? reach : kFarReach;
        const float xSeamLo = kAnchorX + static_cast<float>(i0) * kStep;        // ascending (-X) seam
        const float xSeamHi = kAnchorX + static_cast<float>(i1 + 1) * kStep;    // descending (+X) seam
        for (int dir = 0; dir < 2; ++dir) {
            const float xs = dir ? xSeamHi : xSeamLo;
            const float xe = dir ? (camX + farR) : (camX - farR);
            const float span = (xe - xs) / static_cast<float>(kFarSegs);
            for (int sgi = 0; sgi < kFarSegs; ++sgi) {
                const float xa = xs + span * static_cast<float>(sgi);
                const float xb = xa + span;
                const float ya = surface_y(xa), yb2 = surface_y(xb);
                const float fa = detail::fade_at(xa, camX), fb = detail::fade_at(xb, camX);
                if (fa <= 0.004f && fb <= 0.004f) break;   // fully faded -> nothing to draw further out
                detail::face_grad(out, V{xa,ya,z0}, V{xa,ya,z1}, V{xb,yb2,z1}, V{xb,yb2,z0}, 0, 1, 0, 0.92f*fa, 0.92f*fb);                          // top
                detail::face_grad(out, V{xa,ya-kSkirtVis,z0}, V{xb,yb2-kSkirtVis,z0}, V{xb,yb2,z0}, V{xa,ya,z0}, 0, 0, -1, 0.34f*fa, 0.34f*fb);     // skirt (-Z)
                detail::face_grad(out, V{xb,yb2-kSkirtVis,z1}, V{xa,ya-kSkirtVis,z1}, V{xa,ya,z1}, V{xb,yb2,z1}, 0, 0, 1, 0.34f*fb, 0.34f*fa);      // skirt (+Z)
                detail::face_grad(out, V{xa,ya-kSkirtVis,z1}, V{xa,ya-kSkirtVis,z0}, V{xb,yb2-kSkirtVis,z0}, V{xb,yb2-kSkirtVis,z1}, 0, -1, 0, 0.20f*fa, 0.20f*fb);   // underside
            }
        }
    }

    // 3) void collars: at every level the run crosses near the wanderer, the ceiling hole (plane y=4k+3) and the
    //    floor hole above (y=4k+4) expose the 1 m structural slab void — line it with dark warm panels. Panels
    //    are inset 2 cm from the band edges/planes; where they extend past the actual holes they sit inside the
    //    enclosed slab (invisible), so the analytic span needs no cell quantization.
    {
        const float zi0 = z0 + 0.02f, zi1 = z1 - 0.02f;
        const int kLo = static_cast<int>(std::floor(((kAnchorX - camX) - kCollarReach - 3.0f) / 4.0f));
        const int kHi = static_cast<int>(std::ceil(((kAnchorX - camX) + kCollarReach - 3.0f) / 4.0f));
        for (int k = kLo; k <= kHi; ++k) {
            const float v0 = 4.0f * static_cast<float>(k) + 3.0f;   // void bottom = level k's ceiling plane
            const float v1 = v0 + 1.0f;                             // void top    = level k+1's floor plane
            const float xC = kAnchorX - v0;                         // run crosses the ceiling plane here
            const float u0 = xC - 1.0f - kHoleHalfW - 0.7f;         // union of both holes + margin (floor hole is 1 m -X)
            const float u1 = xC + kHoleHalfW + 0.7f;
            detail::face(out, V{u0,v0,zi0}, V{u0,v1,zi0}, V{u1,v1,zi0}, V{u1,v0,zi0}, 0, 0, 1, 0.18f);    // lining, band -Z side
            detail::face(out, V{u1,v0,zi1}, V{u1,v1,zi1}, V{u0,v1,zi1}, V{u0,v0,zi1}, 0, 0, -1, 0.18f);   // lining, band +Z side
            // Top/bottom plates close the view into the void — but the TRAVEL CORRIDOR must stay open: the run
            // (and the wanderer riding it) passes through every plate plane. Emit each plate as two strips
            // flanking the beam crossing at THAT plane's height (feet cross at xP, the head ~1.8 m of run later),
            // so the aligned holes + beam read as one continuous open diagonal shaft — the "infinite" sightline.
            auto plate = [&](float py, float ny, float shade) {
                const float xP = kAnchorX - py;                     // beam-top crossing at this plane
                const float c0 = xP - 0.5f, c1 = xP + 2.5f;         // corridor: beam body + wanderer passage
                const float aE = (c0 < u1) ? c0 : u1;               // strip A = [u0, min(c0,u1)]
                const float bS = (c1 > u0) ? c1 : u0;               // strip B = [max(c1,u0), u1]
                if (aE - u0 > 0.05f)
                    detail::face(out, V{u0,py,zi0}, V{aE,py,zi0}, V{aE,py,zi1}, V{u0,py,zi1}, 0, ny, 0, shade);
                if (u1 - bS > 0.05f)
                    detail::face(out, V{bS,py,zi0}, V{u1,py,zi0}, V{u1,py,zi1}, V{bS,py,zi1}, 0, ny, 0, shade);
            };
            plate(v1 - 0.02f, -1.0f, 0.12f);   // top plate (seen looking UP through the hole)
            plate(v0 + 0.02f,  1.0f, 0.15f);   // bottom plate (seen looking DOWN)
        }
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
