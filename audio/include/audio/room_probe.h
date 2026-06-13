#pragma once
//
// audio/room_probe.h — raycast room-size probe that sizes the reverb.
//
#include <vector>

#include "contracts/audio_events_v1.h"
#include "contracts/geometry_v1.h"

namespace br::audio {

// Estimate a reverb time (RT60-ish, seconds) for the space around the listener
// by casting horizontal rays at the wall AABBs and averaging the distance to the
// nearest wall. A tight corridor reverberates briefly; a vast hall rings longer.
// Pure + deterministic; result clamped to a sane band.
float probe_reverb_seconds(const contracts::AudioListener& listener,
                           const std::vector<contracts::BoxInstance>& walls);

// The mean free path (avg nearest-wall distance, metres) the reverb time is
// derived from — exposed for tests/telemetry.
float probe_mean_free_path(const contracts::AudioListener& listener,
                           const std::vector<contracts::BoxInstance>& walls);

}  // namespace br::audio
