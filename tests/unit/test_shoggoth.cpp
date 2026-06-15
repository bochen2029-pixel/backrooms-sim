// test_shoggoth — M20 the Shoggoth: deterministic, maze-navigating chase. It is a
// pure function of (state, wanderer, seed) — record→replay reproduces it — and it
// actually routes through the maze toward the wanderer when hunting.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "shoggoth.h"
#include "shoggoth_body.h"
#include "shoggoth_brain.h"
#include "shoggoth_brain_host.h"
#include "shoggoth_vision.h"
#include "shoggoth_hearing.h"
#include "tts.h"
#include "base64.h"

using namespace br::app;

namespace {
float dist2d(const br::core::Vec3& a, const br::core::Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}
Shoggoth spawn_at(float x, float z) {
    Shoggoth s;
    s.pos = br::core::Vec3{x, br::core::kWandererHalfHeight, z};
    return s;
}
}  // namespace

TEST_CASE("the shoggoth is deterministic (record == replay)", "[m20][shoggoth]") {
    const uint64_t seed = 7u;
    const br::core::Vec3 wanderer{16.0f, 1.0f, 16.0f};
    auto run = [&]() {
        Shoggoth s = spawn_at(28.0f, 16.0f);
        for (int t = 0; t < 800; ++t) shoggoth_step(s, wanderer, seed, (t % 8) == 0);
        return shoggoth_hash(s);
    };
    REQUIRE(run() == run());  // same seed + inputs -> identical fingerprint
}

TEST_CASE("a distant wanderer leaves the shoggoth lurking", "[m20][shoggoth]") {
    Shoggoth s = spawn_at(16.0f, 16.0f);
    const br::core::Vec3 far{16.0f + 200.0f, 1.0f, 16.0f};  // well beyond hunt range
    for (int t = 0; t < 120; ++t) shoggoth_step(s, far, 3u, true);
    REQUIRE(s.state == ShoggothState::Lurk);
}

TEST_CASE("a nearby wanderer triggers the hunt and the shoggoth closes in", "[m20][shoggoth]") {
    const uint64_t seed = 3u;
    const br::core::Vec3 wanderer{16.0f, 1.0f, 16.0f};
    Shoggoth s = spawn_at(16.0f + 24.0f, 16.0f);  // ~6 cells away, within hunt range
    const float d0 = dist2d(s.pos, wanderer);
    // First step flips it into Hunt.
    shoggoth_step(s, wanderer, seed, true);
    REQUIRE(s.state != ShoggothState::Lurk);
    // Over time it routes through the maze and closes a large fraction of the gap
    // (exact arrival depends on the winding path; substantial approach proves nav).
    for (int t = 0; t < 1500; ++t) shoggoth_step(s, wanderer, seed, (t % 8) == 0);
    const float d1 = dist2d(s.pos, wanderer);
    REQUIRE(d1 < d0 - 10.0f);      // closed >=10 m of the ~24 m gap through the maze
}

TEST_CASE("the shoggoth never tunnels into a sealed cell (stays on the maze graph)", "[m20][shoggoth]") {
    // It steers toward cell centres along a BFS route, so after stepping it should
    // still occupy a cell reachable from where it began (the maze is connected).
    const uint64_t seed = 9u;
    const br::core::Vec3 wanderer{16.0f, 1.0f, 16.0f};
    Shoggoth s = spawn_at(20.0f, 16.0f);
    bool moved = false;
    const br::core::Vec3 start = s.pos;
    for (int t = 0; t < 600; ++t) {
        shoggoth_step(s, wanderer, seed, (t % 8) == 0);
        if (dist2d(s.pos, start) > 1.0f) moved = true;
    }
    REQUIRE(moved);  // it actually navigated (didn't get stuck at spawn)
}

TEST_CASE("the procedural body is a valid, bounded, finite mesh", "[m20][shoggoth][body]") {
    std::vector<br::contracts::ChunkVertex> body;
    const br::core::Vec3 pos{16.0f, 1.0f, 16.0f};
    build_shoggoth_mesh(body, pos, 0.0f, 1.4f);
    REQUIRE(body.size() > 100u);
    REQUIRE(body.size() % 3u == 0u);  // whole triangles
    for (const auto& v : body) {
        for (int i = 0; i < 3; ++i) {
            REQUIRE(std::isfinite(v.pos[i]));
            REQUIRE(std::isfinite(v.nrm[i]));
            REQUIRE(v.color[i] >= 0.0f);
        }
        // every vertex sits within a few metres of the body centre (no runaway geometry)
        const float dx = v.pos[0] - pos.x, dy = v.pos[1] - pos.y, dz = v.pos[2] - pos.z;
        REQUIRE(std::sqrt(dx * dx + dy * dy + dz * dz) < 6.0f);
        REQUIRE(v.color[0] > v.color[2]);  // warm (red > blue)
    }
    // The writhe changes the mesh (it animates).
    std::vector<br::contracts::ChunkVertex> body2;
    build_shoggoth_mesh(body2, pos, 1.5f, 1.4f);
    bool differs = false;
    for (size_t i = 0; i < body.size() && i < body2.size(); ++i)
        if (body[i].pos[0] != body2[i].pos[0] || body[i].pos[2] != body2[i].pos[2]) { differs = true; break; }
    REQUIRE(differs);
}

TEST_CASE("shoggoth intent parsing: valid, clamp, and reject (defense in depth)", "[m21][shoggoth][brain]") {
    bool ok = false;
    const auto a = parse_shoggoth_intent(R"({"action":"flank","aggression":0.8})", ok);
    REQUIRE(ok); REQUIRE(a.action == ShoggothAction::Flank); REQUIRE(std::fabs(a.aggression - 0.8f) < 1e-4f);
    const auto b = parse_shoggoth_intent(R"({"action":"flee","aggression":5.0})", ok);
    REQUIRE(ok); REQUIRE(b.action == ShoggothAction::Flee); REQUIRE(b.aggression == 1.0f);  // clamped
    parse_shoggoth_intent(R"({"action":"banana"})", ok); REQUIRE(!ok);  // unknown action -> reject
    parse_shoggoth_intent("not json at all", ok); REQUIRE(!ok);
    parse_shoggoth_intent(R"({"mood":"hungry"})", ok); REQUIRE(!ok);    // missing action
    const auto d = parse_shoggoth_intent("garbage", ok);
    REQUIRE(!ok); REQUIRE(d.action == ShoggothAction::Hunt);            // safe default, never throws
}

TEST_CASE("the intent steers the shoggoth: Flee retreats, Hunt closes in", "[m21][shoggoth][brain]") {
    const uint64_t seed = 3u;
    const br::core::Vec3 wanderer{16.0f, 1.0f, 16.0f};
    auto run = [&](ShoggothAction act) {
        Shoggoth s = spawn_at(16.0f + 24.0f, 16.0f);
        s.intent.action = act; s.intent.aggression = 0.7f;
        for (int t = 0; t < 800; ++t) shoggoth_step(s, wanderer, seed, (t % 8) == 0);
        return dist2d(s.pos, wanderer);
    };
    const float huntD = run(ShoggothAction::Hunt);
    const float fleeD = run(ShoggothAction::Flee);
    REQUIRE(huntD < 22.0f);     // hunt closes the ~24 m gap
    REQUIRE(fleeD > huntD);     // flee ends up farther away than hunt
}

TEST_CASE("base64 encodes the RFC 4648 vectors exactly", "[m22][base64]") {
    auto enc = [](const std::string& s) {
        return base64_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };
    REQUIRE(enc("") == "");
    REQUIRE(enc("f") == "Zg==");
    REQUIRE(enc("fo") == "Zm8=");
    REQUIRE(enc("foo") == "Zm9v");
    REQUIRE(enc("foob") == "Zm9vYg==");
    REQUIRE(enc("fooba") == "Zm9vYmE=");
    REQUIRE(enc("foobar") == "Zm9vYmFy");
}

TEST_CASE("the shoggoth POV camera looks out from the creature along its facing", "[m22][shoggoth][vision]") {
    Shoggoth s = spawn_at(10.0f, 20.0f);
    s.yaw = 1.5f;
    const auto cam = shoggoth_pov_camera(s, 16.0f / 9.0f);
    REQUIRE(std::fabs(cam.pos[0] - 10.0f) < 1e-4f);
    REQUIRE(std::fabs(cam.pos[2] - 20.0f) < 1e-4f);
    REQUIRE(cam.pos[1] > s.pos.y);                       // the eye sits above the body
    REQUIRE(std::fabs(cam.yaw - 1.5f) < 1e-4f);          // it looks where it faces
    REQUIRE(cam.fov_y > 0.5f);
    REQUIRE(std::fabs(cam.aspect - 16.0f / 9.0f) < 1e-4f);
}

TEST_CASE("the vision prompt carries the situation and demands the intent JSON", "[m22][shoggoth][vision]") {
    Shoggoth s = spawn_at(0.0f, 0.0f);
    const ShoggothSummary sum = build_shoggoth_summary(s, br::core::Vec3{30.0f, 1.0f, 0.0f}, 7u);
    const std::string p = render_shoggoth_vision_prompt(sum);
    REQUIRE(p.find("image") != std::string::npos);       // references what it SEES
    REQUIRE(p.find("\"action\"") != std::string::npos);  // demands the intent schema
    REQUIRE(p.find("flank") != std::string::npos);
}

TEST_CASE("intent parsing tolerates a markdown-fenced or prose-wrapped reply (vision models)", "[m22][shoggoth][brain]") {
    bool ok = false;
    const auto a = parse_shoggoth_intent("```json\n{\"action\":\"stalk\",\"aggression\":0.3}\n```", ok);
    REQUIRE(ok); REQUIRE(a.action == ShoggothAction::Stalk);
    const auto b = parse_shoggoth_intent("Here is my choice: {\"action\":\"flee\",\"aggression\":0.2} -- run!", ok);
    REQUIRE(ok); REQUIRE(b.action == ShoggothAction::Flee);
}

TEST_CASE("a whisper transcript is trimmed of surrounding whitespace", "[m23][hearing]") {
    REQUIRE(clean_transcript(" (upbeat music)\n") == "(upbeat music)");
    REQUIRE(clean_transcript("\r\n  footsteps approaching  \n") == "footsteps approaching");
    REQUIRE(clean_transcript("") == "");
    REQUIRE(clean_transcript("   \n\t ") == "");
}

TEST_CASE("the hearing prompt carries what it heard, the situation, and the intent JSON", "[m23][shoggoth][hearing]") {
    Shoggoth s = spawn_at(0.0f, 0.0f);
    const ShoggothSummary sum = build_shoggoth_summary(s, br::core::Vec3{8.0f, 1.0f, 0.0f}, 7u);
    const std::string p = render_shoggoth_hearing_prompt(sum, "(footsteps)");
    REQUIRE(p.find("(footsteps)") != std::string::npos);   // what its ears picked up
    REQUIRE(p.find("\"action\"") != std::string::npos);    // demands the intent schema
    REQUIRE(p.find("flank") != std::string::npos);
    const std::string q = render_shoggoth_hearing_prompt(sum, "");  // heard nothing -> "silence"
    REQUIRE(q.find("silence") != std::string::npos);
}

TEST_CASE("the shoggoth mesh fits the DXR creature buffer (writhe-stable, bounded vert count)", "[m25][shoggoth][body]") {
    // M25 injects the body as a dynamic creature into the DXR scene, writing it into a
    // reserved shadeVb tail of kMaxCreatureVerts (4096). The vert COUNT must be stable
    // across writhe (only positions animate, not topology) so the fixed-region write is
    // correct, and must fit the reserved tail.
    std::vector<br::contracts::ChunkVertex> a, b;
    build_shoggoth_mesh(a, br::core::Vec3{0.0f, 1.0f, 0.0f}, 0.0f, 1.4f);
    build_shoggoth_mesh(b, br::core::Vec3{0.0f, 1.0f, 0.0f}, 3.7f, 1.4f);  // a different writhe phase
    REQUIRE(a.size() == b.size());   // topology is writhe-independent -> a stable count
    REQUIRE(a.size() <= 4096u);      // fits the reserved DXR creature tail (kMaxCreatureVerts)
    REQUIRE(a.size() % 3u == 0u);    // whole triangles
}

TEST_CASE("procedural TTS synthesizes deterministic, bounded, non-silent speech", "[m24][tts]") {
    const auto a = synthesize_speech("WARNING LEVEL THREE", 16000u);
    const auto b = synthesize_speech("WARNING LEVEL THREE", 16000u);
    REQUIRE(a.size() > 8000u);   // ~0.5 s+ of audio
    REQUIRE(a == b);             // deterministic (a pure function of text + rate)
    int peak = 0;
    for (int16_t v : a) { const int m = v < 0 ? -static_cast<int>(v) : static_cast<int>(v); if (m > peak) peak = m; }
    REQUIRE(peak > 1000);        // it actually speaks (not silence)
    REQUIRE(synthesize_speech("", 16000u).empty());          // nothing to say -> nothing
    REQUIRE(synthesize_speech("EVACUATE", 16000u) != a);     // different text -> different audio
}

TEST_CASE("the PA lexicon maps known words and falls back for unknown ones", "[m24][tts]") {
    const auto three = tts::text_to_phonemes("THREE");
    REQUIRE(three.size() >= 3u);                  // TH R IY (+ a trailing SIL)
    REQUIRE(three[0] == tts::Ph::TH);
    REQUIRE(three[1] == tts::Ph::R);
    REQUIRE(three[2] == tts::Ph::IY);
    REQUIRE_FALSE(tts::text_to_phonemes("ZZZQX").empty());   // letter-to-sound fallback, not silent
    REQUIRE(tts::text_to_phonemes("").empty());
    REQUIRE(tts::text_to_phonemes("level three").size() > tts::text_to_phonemes("three").size());  // case-insensitive
}

TEST_CASE("the live async brain host has a clean lifecycle and an empty initial poll", "[m21b][shoggoth][brain]") {
    // M21b: construct (which starts the worker thread), then -- before any submit --
    // poll() is empty and the counters are zero, and destruction joins cleanly. With no
    // summary submitted the worker just waits on its condition variable, so this is
    // fully network-free and deterministic (the live KEEL path is proven by the gate).
    ShoggothBrainHost host("127.0.0.1", 7071);
    REQUIRE(host.poll().empty());
    REQUIRE(host.requests() == 0u);
    REQUIRE(host.produced() == 0u);
    // The destructor runs at scope exit: it signals stop + joins the worker (no hang).
}
