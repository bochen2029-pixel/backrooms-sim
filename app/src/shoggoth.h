#pragma once
//
// app/shoggoth.h — The Shoggoth: a deterministic, maze-navigating chase creature (M20).
//
// Lives OUTSIDE WorldState (so every existing replay/walk-bot/Director hash is
// untouched) but is fully deterministic — seeded, no wall-clock, its own hash — so a
// record→replay reproduces it bit-for-bit (the M21 sacred gate to come). It BFS-
// pathfinds the Level-0 maze (gen layout) toward the wanderer when hunting, oozes
// organically, and runs an intermittent chase state machine (lurk→hunt→chase→retreat).
// The body is a proxy (a writhing blob, rendered elsewhere); the KEEL brain is M21.
//
#include <cmath>
#include <cstdint>
#include <queue>
#include <set>
#include <unordered_map>

#include "core/rng.h"
#include "core/world.h"
#include "gen/layout.h"
#include "contracts/chunk_gen_v1.h"

namespace br::app {

enum class ShoggothState { Lurk = 0, Hunt = 1, Chase = 2, Retreat = 3 };

// M21: the high-level *intent* the KEEL brain sets (the cascade's top layer). The
// deterministic navigator below executes it. `Hunt` is the default (= the M20
// behavior, so the brain being off changes nothing).
enum class ShoggothAction { Hunt = 0, Stalk = 1, Lurk = 2, Flank = 3, Flee = 4 };

struct ShoggothIntent {
    ShoggothAction action = ShoggothAction::Hunt;
    float aggression = 0.5f;  // 0..1, scales speed
};

struct Shoggoth {
    br::core::Vec3 pos{0.0f, 0.0f, 0.0f};
    int32_t level = 0;     // M29: the floor it patrols (derived from pos.y; it never crosses a seam)
    float yaw = 0.0f;      // facing (radians)
    float writhe = 0.0f;   // organic body phase
    ShoggothState state = ShoggothState::Lurk;
    uint32_t state_ticks = 0;
    int64_t wander_gi = 0, wander_gj = 0;  // current lurk goal cell
    ShoggothIntent intent;                 // M21: set by the KEEL brain via the event log
    br::core::Pcg64 rng{0x5170990000000001ull};
};

// --- maze queries (global cell grid; a cell is kCellSize = 4 m) ----------------
inline int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
inline void world_to_cell(float x, float z, int64_t& gi, int64_t& gj) {
    gi = static_cast<int64_t>(std::floor(x / br::gen::kCellSize));
    gj = static_cast<int64_t>(std::floor(z / br::gen::kCellSize));
}
inline float cell_center(int64_t g) { return (static_cast<float>(g) + 0.5f) * br::gen::kCellSize; }

// Is the move from global cell (gi,gj) in `dir` (0=E+x,1=W-x,2=N+z,3=S-z) open?
// `cache` memoises per-chunk layouts so a BFS generates each chunk's maze once.
inline bool maze_open(uint64_t seed, int64_t gi, int64_t gj, int dir,
                      std::unordered_map<int64_t, br::gen::ChunkLayout>& cache, int32_t level = 0) {
    const int G = br::gen::kCellsPerChunk;
    const int64_t cx = floor_div(gi, G), cz = floor_div(gj, G);
    const int li = static_cast<int>(gi - cx * G), lj = static_cast<int>(gj - cz * G);
    const int64_t key = (cx & 0xffffffffLL) | (cz << 32);
    auto it = cache.find(key);
    if (it == cache.end())
        it = cache.emplace(key, br::gen::generate_layout(seed, contracts::ChunkKey{level, cx, cz})).first;
    const br::gen::ChunkLayout& L = it->second;
    switch (dir) {
        case 0: return !L.vwall[li + 1][lj];  // east
        case 1: return !L.vwall[li][lj];      // west
        case 2: return !L.hwall[li][lj + 1];  // north (+z)
        default: return !L.hwall[li][lj];     // south (-z)
    }
}

// BFS over a bounded cell window: the direction (0..3) to step from (sgi,sgj) toward
// (tgi,tgj), or -1 if already there / unreachable within the window. Deterministic
// (neighbours are visited in a fixed E,W,N,S order).
inline int next_step_dir(uint64_t seed, int32_t level, int64_t sgi, int64_t sgj, int64_t tgi, int64_t tgj) {
    if (sgi == tgi && sgj == tgj) return -1;
    constexpr int R = 22;  // window radius in cells (~88 m)
    std::unordered_map<int64_t, br::gen::ChunkLayout> cache;
    std::set<std::pair<int64_t, int64_t>> seen;
    std::queue<std::pair<std::pair<int64_t, int64_t>, int>> q;  // (cell, first-step dir)
    seen.insert({sgi, sgj});
    static const int dgi[4] = {1, -1, 0, 0};
    static const int dgj[4] = {0, 0, 1, -1};
    for (int d = 0; d < 4; ++d) {
        if (maze_open(seed, sgi, sgj, d, cache, level)) {
            const int64_t ni = sgi + dgi[d], nj = sgj + dgj[d];
            if (ni == tgi && nj == tgj) return d;
            if (seen.insert({ni, nj}).second) q.push({{ni, nj}, d});
        }
    }
    while (!q.empty()) {
        const auto cur = q.front(); q.pop();
        const int64_t ci = cur.first.first, cj = cur.first.second;
        const int first = cur.second;
        for (int d = 0; d < 4; ++d) {
            if (!maze_open(seed, ci, cj, d, cache, level)) continue;
            const int64_t ni = ci + dgi[d], nj = cj + dgj[d];
            if (ni == tgi && nj == tgj) return first;
            if (std::llabs(ni - sgi) > R || std::llabs(nj - sgj) > R) continue;
            if (seen.insert({ni, nj}).second) q.push({{ni, nj}, first});
        }
    }
    return -1;  // no path within the window
}

// --- the creature --------------------------------------------------------------
// Distances (m) for the state machine.
constexpr float kShogHuntRange = 44.0f;
constexpr float kShogChaseRange = 9.0f;
constexpr float kShogCatchRange = 2.2f;
constexpr float kShogLoseRange = 70.0f;

inline float shog_speed(ShoggothState s) {
    switch (s) {
        case ShoggothState::Chase: return 5.6f;
        case ShoggothState::Hunt: return 3.3f;
        case ShoggothState::Retreat: return 4.2f;
        default: return 1.1f;  // lurk
    }
}

// Advance the shoggoth one sim tick toward/around the wanderer. Pure + deterministic
// given (shoggoth, wanderer, seed). `pathfind` is true when it's time to recompute
// the route (cheap to call every few ticks; the navigator is bounded BFS).
inline void shoggoth_step(Shoggoth& sh, const br::core::Vec3& wanderer, uint64_t seed, bool pathfind) {
    const float dt = br::core::kTickDt;
    const float dx = wanderer.x - sh.pos.x, dz = wanderer.z - sh.pos.z;
    sh.level = contracts::level_from_y(sh.pos.y);  // M29: the floor it patrols (no vertical motion below)
    const bool same_level = (contracts::level_from_y(wanderer.y) == sh.level);
    // M29: it can't sense prey on another floor -> descending/ascending a stair ESCAPES it. At level 0
    // (every M20/M21 scene the wanderer + creature share) same_level is always true, so behaviour is unchanged.
    const float dist = same_level ? std::sqrt(dx * dx + dz * dz) : (kShogLoseRange + 1.0f);

    ++sh.state_ticks;
    const ShoggothState was = sh.state;
    switch (sh.state) {
        case ShoggothState::Lurk:
            if (dist < kShogHuntRange) sh.state = ShoggothState::Hunt;
            break;
        case ShoggothState::Hunt:
            if (dist < kShogChaseRange) sh.state = ShoggothState::Chase;
            else if (dist > kShogLoseRange) sh.state = ShoggothState::Lurk;
            break;
        case ShoggothState::Chase:
            if (dist < kShogCatchRange) sh.state = ShoggothState::Retreat;
            else if (dist > kShogChaseRange * 2.0f) sh.state = ShoggothState::Hunt;
            break;
        case ShoggothState::Retreat:
            if (sh.state_ticks > 300u) sh.state = ShoggothState::Hunt;  // ~2.5 s, then resume
            break;
    }
    if (sh.state != was) sh.state_ticks = 0;

    int64_t sgi, sgj; world_to_cell(sh.pos.x, sh.pos.z, sgi, sgj);
    int64_t wgi, wgj; world_to_cell(wanderer.x, wanderer.z, wgi, wgj);

    // Pick the navigation goal cell from the state.
    int64_t tgi = sgi, tgj = sgj;
    if (sh.state == ShoggothState::Hunt || sh.state == ShoggothState::Chase) {
        tgi = wgi; tgj = wgj;
    } else if (sh.state == ShoggothState::Retreat) {
        tgi = sgi + (sgi >= wgi ? 3 : -3);
        tgj = sgj + (sgj >= wgj ? 3 : -3);
    } else {  // lurk: amble to a random nearby cell, repick when reached or periodically
        if (pathfind && ((sgi == sh.wander_gi && sgj == sh.wander_gj) || (sh.state_ticks % 360u == 0u))) {
            sh.wander_gi = sgi + (static_cast<int64_t>(sh.rng.next_u64() % 7u) - 3);
            sh.wander_gj = sgj + (static_cast<int64_t>(sh.rng.next_u64() % 7u) - 3);
        }
        tgi = sh.wander_gi; tgj = sh.wander_gj;
    }

    // M21: the KEEL intent overrides/modulates the goal (Hunt = the M20 default, so
    // the brain being off changes nothing).
    switch (sh.intent.action) {
        case ShoggothAction::Hunt: break;                                   // state-machine goal
        case ShoggothAction::Stalk: tgi = wgi; tgj = wgj; break;            // approach (slowed below)
        case ShoggothAction::Lurk: tgi = sh.wander_gi; tgj = sh.wander_gj; break;
        case ShoggothAction::Flank: {                                       // circle: rotate the approach 90°
            const int64_t di = wgi - sgi, dj = wgj - sgj;
            tgi = wgi - dj / 2; tgj = wgj + di / 2;
            break;
        }
        case ShoggothAction::Flee:
            tgi = sgi + (sgi >= wgi ? 4 : -4);
            tgj = sgj + (sgj >= wgj ? 4 : -4);
            break;
    }

    // Steer toward the centre of the next cell along the BFS route (stays in corridors).
    float ddx = 0.0f, ddz = 0.0f;
    const int dir = next_step_dir(seed, sh.level, sgi, sgj, tgi, tgj);
    if (dir >= 0) {
        static const int dgi[4] = {1, -1, 0, 0};
        static const int dgj[4] = {0, 0, 1, -1};
        const float cx = cell_center(sgi + dgi[dir]), cz = cell_center(sgj + dgj[dir]);
        ddx = cx - sh.pos.x; ddz = cz - sh.pos.z;
    } else {
        // No route (or arrived): drift toward the current cell centre.
        ddx = cell_center(sgi) - sh.pos.x; ddz = cell_center(sgj) - sh.pos.z;
    }
    const float dl = std::sqrt(ddx * ddx + ddz * ddz);
    if (dl > 1e-4f) { ddx /= dl; ddz /= dl; sh.yaw = std::atan2(ddx, ddz); }

    // Organic ooze: speed pulses with the writhe so it never glides robotically.
    // M21: the intent scales it (Stalk creeps; aggression 0.5 -> x1.0 = M20 default).
    float speed = shog_speed(sh.state);
    if (sh.intent.action == ShoggothAction::Stalk) speed *= 0.55f;
    speed *= 0.7f + 0.6f * sh.intent.aggression;
    sh.writhe += dt * (2.5f + speed);
    const float ooze = 0.92f + 0.18f * std::sin(sh.writhe * 1.7f);  // pulses ~[0.74,1.10], avg ~0.92
    sh.pos.x += ddx * speed * ooze * dt;
    sh.pos.z += ddz * speed * ooze * dt;
}

// Deterministic fingerprint of the shoggoth (record→replay must match).
inline uint64_t shoggoth_hash(const Shoggoth& sh) {
    uint64_t h = 1469598103934665603ull;
    const uint64_t prime = 1099511628211ull;
    auto mix = [&](uint64_t q) { for (int i = 0; i < 8; ++i) { h ^= (q >> (i * 8)) & 0xff; h *= prime; } };
    auto mixf = [&](float f) { uint32_t u; std::memcpy(&u, &f, 4); mix(u); };
    mixf(sh.pos.x); mixf(sh.pos.y); mixf(sh.pos.z); mixf(sh.yaw); mixf(sh.writhe);
    mix(static_cast<uint64_t>(sh.state)); mix(sh.state_ticks);
    mix(static_cast<uint64_t>(sh.wander_gi)); mix(static_cast<uint64_t>(sh.wander_gj));
    mix(static_cast<uint64_t>(sh.level));  // M29: the floor it patrols
    mix(static_cast<uint64_t>(sh.intent.action)); mixf(sh.intent.aggression);  // M21 intent
    return h;
}

}  // namespace br::app
