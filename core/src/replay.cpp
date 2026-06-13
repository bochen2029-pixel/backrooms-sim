#include "core/replay.h"

#include <cstdio>

namespace br::core {

bool write_replay(const std::string& path, const Replay& r) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    contracts::ReplayHeader h{};
    h.magic = contracts::kReplayMagic;
    h.version = contracts::kReplayVersion;
    h.world_seed = r.world_seed;
    h.tick_count = static_cast<uint64_t>(r.commands.size());

    bool ok = std::fwrite(&h, sizeof(h), 1u, f) == 1u;
    if (ok && !r.commands.empty()) {
        const size_t n = r.commands.size();
        ok = std::fwrite(r.commands.data(), sizeof(contracts::InputCommand), n, f) == n;
    }
    std::fclose(f);
    return ok;
}

bool read_replay(const std::string& path, Replay& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    contracts::ReplayHeader h{};
    bool ok = std::fread(&h, sizeof(h), 1u, f) == 1u;
    if (!ok || h.magic != contracts::kReplayMagic || h.version != contracts::kReplayVersion) {
        std::fclose(f);
        return false;  // E_REPLAY_VERSION / corrupt
    }

    out.world_seed = h.world_seed;
    const size_t n = static_cast<size_t>(h.tick_count);
    out.commands.resize(n);
    if (n > 0) {
        ok = std::fread(out.commands.data(), sizeof(contracts::InputCommand), n, f) == n;
    }
    std::fclose(f);
    return ok;
}

}  // namespace br::core
