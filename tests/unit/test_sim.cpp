// test_sim — per-tick determinism (INV-1) and replay round-trip / reproduction.
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <vector>

#include "core/rng.h"
#include "core/world.h"
#include "core/replay.h"

using namespace br::core;
namespace contracts = br::contracts;

namespace {
// A deterministic scripted input stream that exercises movement + jumps.
std::vector<contracts::InputCommand> make_stream(uint64_t seed, int ticks) {
    Pcg64 r(seed);
    std::vector<contracts::InputCommand> cmds;
    cmds.reserve(static_cast<size_t>(ticks));
    for (int i = 0; i < ticks; ++i) {
        contracts::InputCommand c{};
        c.move_x = static_cast<float>(r.next_double() * 2.0 - 1.0);
        c.move_z = static_cast<float>(r.next_double() * 2.0 - 1.0);
        c.look_yaw = static_cast<float>((r.next_double() - 0.5) * 0.10);
        c.look_pitch = static_cast<float>((r.next_double() - 0.5) * 0.04);
        c.buttons = (r.next_double() < 0.02) ? contracts::kButtonJump : uint8_t{0};
        cmds.push_back(c);
    }
    return cmds;
}

std::vector<uint64_t> run_hashes(uint64_t seed, const std::vector<contracts::InputCommand>& cmds) {
    WorldState s(seed);
    std::vector<uint64_t> hashes;
    hashes.reserve(cmds.size());
    for (const auto& c : cmds) {
        tick(s, c);
        hashes.push_back(world_state_hash(s));
    }
    return hashes;
}
}  // namespace

TEST_CASE("per-tick state hash is identical across two runs", "[sim][determinism]") {
    const auto cmds = make_stream(0xC0FFEEu, 2000);
    const auto a = run_hashes(777u, cmds);
    const auto b = run_hashes(777u, cmds);
    REQUIRE(a == b);            // whole per-tick hash sequence matches
    REQUIRE(a.size() == 2000u);
}

TEST_CASE("different seed diverges the hash sequence", "[sim][determinism]") {
    const auto cmds = make_stream(0xC0FFEEu, 500);
    const auto a = run_hashes(1u, cmds);
    const auto b = run_hashes(2u, cmds);
    REQUIRE(a != b);
}

TEST_CASE("replay write/read round-trips exactly", "[sim][replay]") {
    Replay r;
    r.world_seed = 42u;
    r.commands = make_stream(0xABCDu, 256);
    const char* path = "test_roundtrip.replay";
    REQUIRE(write_replay(path, r));

    Replay back;
    REQUIRE(read_replay(path, back));
    REQUIRE(back.world_seed == r.world_seed);
    REQUIRE(back.commands.size() == r.commands.size());
    bool same = true;
    for (size_t i = 0; i < r.commands.size(); ++i) {
        const auto& x = r.commands[i];
        const auto& y = back.commands[i];
        if (x.move_x != y.move_x || x.move_z != y.move_z ||
            x.look_yaw != y.look_yaw || x.look_pitch != y.look_pitch ||
            x.buttons != y.buttons) {
            same = false;
            break;
        }
    }
    REQUIRE(same);
    std::remove(path);
}

TEST_CASE("replay reproduces the final state hash", "[sim][replay][determinism]") {
    const auto cmds = make_stream(0x1234u, 1500);
    const auto live = run_hashes(99u, cmds);

    Replay r;
    r.world_seed = 99u;
    r.commands = cmds;
    const char* path = "test_reproduce.replay";
    REQUIRE(write_replay(path, r));
    Replay back;
    REQUIRE(read_replay(path, back));
    const auto replayed = run_hashes(back.world_seed, back.commands);

    REQUIRE(live == replayed);
    std::remove(path);
}

TEST_CASE("replay read rejects a bad header", "[sim][replay]") {
    const char* path = "test_badhdr.replay";
    std::FILE* f = std::fopen(path, "wb");
    REQUIRE(f != nullptr);
    const char junk[16] = {'n', 'o', 'p', 'e'};
    std::fwrite(junk, 1, sizeof(junk), f);
    std::fclose(f);

    Replay back;
    REQUIRE_FALSE(read_replay(path, back));  // E_REPLAY_VERSION / corrupt
    std::remove(path);
}
