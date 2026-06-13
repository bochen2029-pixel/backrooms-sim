#pragma once
//
// core/lighting.h — deterministic fluorescent flicker (M5). Lives in the sim
// core so it is part of the replayable state: at a given (WorldSeed, light id,
// tick) the flicker is bit-identical, so replays reproduce the lighting (INV-1).
//
#include <cstdint>

namespace br::core {

// Flicker multiplier in [0, 1] for one light. Most lights are steady (1.0);
// a fraction flicker smoothly over time.
float light_flicker(uint64_t world_seed, uint64_t light_id, uint64_t tick) noexcept;

}  // namespace br::core
