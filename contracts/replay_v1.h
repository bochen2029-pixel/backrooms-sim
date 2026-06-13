#pragma once
//
// contracts/replay_v1.h — input + Event Log serialization boundary (v1.0).
//
// The Replay is the recorded input stream that reproduces a run bit-identically
// (INV-1). M2 establishes the per-tick InputCommand and the on-disk replay
// format; the Event Log (committed Directives + sim events) layers on in later
// milestones without breaking this header (additive within v1).
//
#include <cstdint>

namespace br::contracts {

// One tick of wanderer input. Analog axes in [-1, 1]; look deltas in radians.
struct InputCommand {
    float move_x = 0.0f;       // strafe: +right, -left
    float move_z = 0.0f;       // walk:   +forward, -back
    float look_yaw = 0.0f;     // delta radians applied to yaw this tick
    float look_pitch = 0.0f;   // delta radians applied to pitch this tick
    uint8_t buttons = 0;       // bitfield (see kButton*)
    uint8_t _pad[3] = {0, 0, 0};
};

constexpr uint8_t kButtonJump = 0x01;

// On-disk replay = ReplayHeader followed by `tick_count` InputCommand records
// (POD, host-endian; the build targets a single x64 toolchain).
constexpr uint32_t kReplayMagic = 0x4B524242u;   // 'BBRK' (Backrooms Replay)
constexpr uint32_t kReplayVersion = 1u;

struct ReplayHeader {
    uint32_t magic;        // kReplayMagic
    uint32_t version;      // kReplayVersion; mismatch => E_REPLAY_VERSION
    uint64_t world_seed;   // WorldSeed the stream was recorded against
    uint64_t tick_count;   // number of InputCommand records that follow
};

}  // namespace br::contracts
