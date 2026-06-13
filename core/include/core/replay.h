#pragma once
//
// core/replay.h — record/playback of input streams (replay_v1). A Replay plus a
// WorldSeed reproduces a run bit-identically (INV-1). No graphics includes.
//
#include <cstdint>
#include <string>
#include <vector>

#include "contracts/replay_v1.h"

namespace br::core {

struct Replay {
    uint64_t world_seed = 0;
    std::vector<contracts::InputCommand> commands;  // one per tick
};

// Write/read the replay_v1 on-disk format. read_replay returns false on a
// magic/version mismatch (the E_REPLAY_VERSION case) or I/O failure.
bool write_replay(const std::string& path, const Replay& r);
bool read_replay(const std::string& path, Replay& out);

}  // namespace br::core
