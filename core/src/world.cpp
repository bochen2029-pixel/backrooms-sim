#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace br::core {

namespace {
constexpr float kMaxSubstep = 0.05f;  // max move per collision substep (m)
constexpr int kMaxSubsteps = 256;     // cap (handles up to ~1500 m/s)
constexpr float kSkin = 0.001f;       // push-out epsilon to avoid re-overlap
}  // namespace

WorldState::WorldState(uint64_t world_seed) : rng(world_seed) {
    wanderer.pos = test_room_spawn();
}

const std::vector<Aabb>& test_room() {
    static const std::vector<Aabb> boxes = [] {
        std::vector<Aabb> v;
        const float R = 6.0f;   // half-extent of the floor plan
        const float T = 0.5f;   // wall/slab thickness
        const float H = 3.0f;   // room height
        v.push_back({{-R - T, -T, -R - T}, {R + T, 0.0f, R + T}});    // floor
        v.push_back({{-R - T, H, -R - T}, {R + T, H + T, R + T}});    // ceiling
        v.push_back({{-R - T, 0.0f, -R - T}, {R + T, H, -R}});        // -Z wall
        v.push_back({{-R - T, 0.0f, R}, {R + T, H, R + T}});          // +Z wall
        v.push_back({{-R - T, 0.0f, -R}, {-R, H, R}});                // -X wall
        v.push_back({{R, 0.0f, -R}, {R + T, H, R}});                  // +X wall
        v.push_back({{2.0f, 0.0f, 2.0f}, {2.6f, H, 2.6f}});           // pillar A
        v.push_back({{-3.0f, 0.0f, -1.0f}, {-2.4f, H, -0.4f}});       // pillar B
        return v;
    }();
    return boxes;
}

Vec3 test_room_spawn() {
    return {0.0f, kWandererHalfHeight + 0.02f, 0.0f};
}

const std::vector<Aabb>& open_ground() {
    static const std::vector<Aabb> g = {
        Aabb{{-1.0e6f, -1.0f, -1.0e6f}, {1.0e6f, 0.0f, 1.0e6f}},
    };
    return g;
}

std::vector<contracts::BoxInstance> test_room_box_instances() {
    std::vector<contracts::BoxInstance> out;
    out.reserve(test_room().size());
    for (const Aabb& b : test_room()) {
        contracts::BoxInstance bi;
        bi.mn[0] = b.mn.x; bi.mn[1] = b.mn.y; bi.mn[2] = b.mn.z;
        bi.mx[0] = b.mx.x; bi.mx[1] = b.mx.y; bi.mx[2] = b.mx.z;
        out.push_back(bi);
    }
    return out;
}

Vec3 move_and_collide(Vec3 pos, Vec3 he, Vec3& vel, float dt,
                      const std::vector<Aabb>& boxes, bool& on_ground) {
    on_ground = false;
    Vec3 delta = vel * dt;

    float maxc = std::fabs(delta.x);
    maxc = std::max(maxc, std::fabs(delta.y));
    maxc = std::max(maxc, std::fabs(delta.z));
    int steps = 1;
    if (maxc > kMaxSubstep) {
        steps = static_cast<int>(std::ceil(maxc / kMaxSubstep));
        if (steps > kMaxSubsteps) steps = kMaxSubsteps;
    }
    Vec3 step = delta * (1.0f / static_cast<float>(steps));

    for (int s = 0; s < steps; ++s) {
        // Resolve one axis at a time so a blocked axis still slides on the others.
        pos.x += step.x;
        for (const Aabb& b : boxes) {
            if (overlaps(box_at(pos, he), b)) {
                if (step.x > 0.0f) pos.x = b.mn.x - he.x - kSkin;
                else if (step.x < 0.0f) pos.x = b.mx.x + he.x + kSkin;
                vel.x = 0.0f;
                step.x = 0.0f;
            }
        }
        pos.z += step.z;
        for (const Aabb& b : boxes) {
            if (overlaps(box_at(pos, he), b)) {
                if (step.z > 0.0f) pos.z = b.mn.z - he.z - kSkin;
                else if (step.z < 0.0f) pos.z = b.mx.z + he.z + kSkin;
                vel.z = 0.0f;
                step.z = 0.0f;
            }
        }
        pos.y += step.y;
        for (const Aabb& b : boxes) {
            if (overlaps(box_at(pos, he), b)) {
                if (step.y > 0.0f) {
                    pos.y = b.mn.y - he.y - kSkin;  // bonked ceiling
                } else if (step.y < 0.0f) {
                    pos.y = b.mx.y + he.y + kSkin;   // landed on floor
                    on_ground = true;
                }
                vel.y = 0.0f;
                step.y = 0.0f;
            }
        }
    }
    return pos;
}

void tick(WorldState& s, const contracts::InputCommand& in) {
    tick(s, in, test_room());
}

void tick(WorldState& s, const contracts::InputCommand& in,
          const std::vector<Aabb>& collision) {
    Wanderer& w = s.wanderer;

    // Look.
    w.yaw += in.look_yaw;
    w.pitch = clampf(w.pitch + in.look_pitch, -kMaxPitch, kMaxPitch);

    // Horizontal wish direction from yaw + analog move (direct velocity control).
    const float sy = std::sin(w.yaw);
    const float cy = std::cos(w.yaw);
    const Vec3 fwd{sy, 0.0f, cy};
    const Vec3 right{cy, 0.0f, -sy};
    Vec3 wish = fwd * in.move_z + right * in.move_x;
    wish.y = 0.0f;
    const float wl = length(wish);
    if (wl > 1.0f) wish = wish * (1.0f / wl);
    const Vec3 horiz = wish * kWalkSpeed;
    w.vel.x = horiz.x;
    w.vel.z = horiz.z;

    // Gravity + jump.
    w.vel.y -= kGravity * kTickDt;
    if (w.on_ground && (in.buttons & contracts::kButtonJump) != 0) {
        w.vel.y = kJumpSpeed;
    }

    // Integrate with collision.
    const Vec3 he{kWandererRadius, kWandererHalfHeight, kWandererRadius};
    const Vec3 old_pos = w.pos;
    bool grounded = false;
    w.pos = move_and_collide(w.pos, he, w.vel, kTickDt, collision, grounded);
    w.on_ground = grounded;

    // Odometer (horizontal distance only).
    const float dx = w.pos.x - old_pos.x;
    const float dz = w.pos.z - old_pos.z;
    s.odometer += std::sqrt(dx * dx + dz * dz);

    s.tick += 1;
}

uint64_t world_state_hash(const WorldState& s) {
    uint64_t h = 1469598103934665603ULL;
    const uint64_t prime = 1099511628211ULL;
    auto mix_byte = [&](uint8_t b) { h ^= b; h *= prime; };
    auto mix_f = [&](float f) {
        uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        for (int i = 0; i < 4; ++i) mix_byte(static_cast<uint8_t>((u >> (i * 8)) & 0xff));
    };
    auto mix_q = [&](uint64_t q) {
        for (int i = 0; i < 8; ++i) mix_byte(static_cast<uint8_t>((q >> (i * 8)) & 0xff));
    };

    const Wanderer& w = s.wanderer;
    mix_f(w.pos.x); mix_f(w.pos.y); mix_f(w.pos.z);
    mix_f(w.vel.x); mix_f(w.vel.y); mix_f(w.vel.z);
    mix_f(w.yaw); mix_f(w.pitch);
    mix_byte(w.on_ground ? 1u : 0u);
    mix_q(s.tick);
    mix_f(s.odometer);
    mix_q(s.rng.state_hi());
    mix_q(s.rng.state_lo());
    return h;
}

contracts::CameraPose wanderer_camera(const WorldState& s, float aspect) {
    contracts::CameraPose c{};
    const Vec3 eye = s.wanderer.pos + Vec3{0.0f, kEyeHeight, 0.0f};
    c.pos[0] = eye.x; c.pos[1] = eye.y; c.pos[2] = eye.z;
    c.yaw = s.wanderer.yaw;
    c.pitch = s.wanderer.pitch;
    c.fov_y = 1.2217305f;  // ~70 degrees
    c.aspect = aspect;
    return c;
}

}  // namespace br::core
