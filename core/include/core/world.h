#pragma once
//
// core/world.h — WorldState, the fixed-timestep tick, capsule-vs-AABB collision
// with sliding, the hardcoded test room, and per-tick state hashing.
//
// Determinism (INV-1): a WorldState advanced by `tick` is a pure function of
// (seed, input stream). No wall-clock, no unseeded RNG, single-threaded,
// /fp:strict. Zero graphics/audio/director includes (INV-5).
//
#include <cstdint>
#include <vector>

#include "core/aabb.h"
#include "core/math.h"
#include "core/rng.h"
#include "contracts/replay_v1.h"
#include "contracts/world_view_v1.h"

namespace br::core {

// --- Tunables (fixed; part of the determinism contract) --------------------
constexpr float kTickHz = 120.0f;
constexpr float kTickDt = 1.0f / kTickHz;

constexpr float kWandererRadius = 0.35f;      // capsule XZ half-extent
constexpr float kWandererHalfHeight = 0.90f;  // capsule Y half-extent (body 1.8 m)
constexpr float kEyeHeight = 0.70f;           // eye above capsule center
constexpr float kWalkSpeed = 4.0f;            // m/s
constexpr float kGravity = 18.0f;             // m/s^2
constexpr float kJumpSpeed = 6.0f;            // m/s
constexpr float kMaxPitch = 1.55334f;         // ~89 degrees

// --- Entities --------------------------------------------------------------
struct Wanderer {
    Vec3 pos;            // capsule center
    Vec3 vel;
    float yaw = 0.0f;    // radians; 0 looks toward +Z
    float pitch = 0.0f;  // radians
    bool on_ground = false;
};

struct WorldState {
    Wanderer wanderer;
    Pcg64 rng;             // owned RNG (INV-1); seeded from WorldSeed
    uint64_t tick = 0;
    float odometer = 0.0f; // horizontal distance walked (m)

    explicit WorldState(uint64_t world_seed);
};

// --- Test room (single source of truth for sim collision AND rendering) -----
const std::vector<Aabb>& test_room();
Vec3 test_room_spawn();
std::vector<contracts::BoxInstance> test_room_box_instances();

// --- Open ground: a single large floor for the streaming walk (M3). ---------
const std::vector<Aabb>& open_ground();

// --- Collision: move an AABB proxy (half-extents he) by vel*dt against solid
// boxes, resolving per axis with sliding + substepping. Zeroes blocked velocity
// axes; sets on_ground when stopped from below. Returns the new center. ------
Vec3 move_and_collide(Vec3 pos, Vec3 he, Vec3& vel, float dt,
                      const std::vector<Aabb>& boxes, bool& on_ground);

// --- Advance exactly one 120 Hz tick (pure given (state, input, collision)). -
// The 2-arg form collides against the test room; the 3-arg form against any
// supplied collision geometry (e.g. open_ground() for the streaming walk).
void tick(WorldState& s, const contracts::InputCommand& in,
          const std::vector<Aabb>& collision);
void tick(WorldState& s, const contracts::InputCommand& in);

// --- Per-tick state hash: position, velocity, orientation, RNG state, tick. --
uint64_t world_state_hash(const WorldState& s);

// --- Renderer-facing camera pose for the current wanderer. ------------------
contracts::CameraPose wanderer_camera(const WorldState& s, float aspect);

}  // namespace br::core
