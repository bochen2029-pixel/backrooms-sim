#pragma once
//
// app/shoggoth_brain.h — the Shoggoth's KEEL brain (M21): summary → prompt → intent.
//
// Reuses the Director's KEEL HTTP client (`keel_complete`) + JSON reader. The LLM runs
// at RECORD time ONLY; validated intents enter the (deterministic) shoggoth as event-
// log entries at fixed effective_ticks, so a replay with the model OFFLINE reproduces
// the chase bit-for-bit — the sacred gate (the M11/Gate-4 pattern, for the monster).
// Raw model text never drives the creature except through `parse_shoggoth_intent`.
//
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "director/json.h"
#include "shoggoth.h"

namespace br::app {

struct ShoggothSummary {
    int64_t sgi, sgj;   // shoggoth cell
    int64_t wgi, wgj;   // wanderer cell
    float distance_m;
    int state;          // ShoggothState
    uint64_t tick;
};

inline ShoggothSummary build_shoggoth_summary(const Shoggoth& sh, const br::core::Vec3& wanderer, uint64_t tick) {
    ShoggothSummary s{};
    world_to_cell(sh.pos.x, sh.pos.z, s.sgi, s.sgj);
    world_to_cell(wanderer.x, wanderer.z, s.wgi, s.wgj);
    const float dx = wanderer.x - sh.pos.x, dz = wanderer.z - sh.pos.z;
    s.distance_m = std::sqrt(dx * dx + dz * dz);
    s.state = static_cast<int>(sh.state);
    s.tick = tick;
    return s;
}

inline std::string render_shoggoth_prompt(const ShoggothSummary& s) {
    static const char* kStates[4] = {"lurking", "hunting", "chasing", "retreating"};
    char buf[1100];
    std::snprintf(buf, sizeof(buf),
        "You are a SHOGGOTH - a vast, amorphous, intelligent horror loose in the infinite "
        "Backrooms, hunting a lone wanderer. You are patient, cruel, and curious. Given your "
        "situation, choose ONE behaviour and emit EXACTLY ONE compact JSON object and nothing else:\n"
        "{\"action\":\"hunt|stalk|lurk|flank|flee\",\"aggression\":<0.0-1.0>}\n"
        "  hunt = close in directly;  stalk = creep closer slowly;  lurk = wait / withdraw;\n"
        "  flank = circle around to cut it off;  flee = retreat (rare). Output ONLY the JSON.\n\n"
        "Situation: you are %s, %.0f m from the wanderer. You are at cell (%lld,%lld); it is at "
        "(%lld,%lld). Tick %llu.",
        kStates[(s.state >= 0 && s.state < 4) ? s.state : 0], static_cast<double>(s.distance_m),
        static_cast<long long>(s.sgi), static_cast<long long>(s.sgj),
        static_cast<long long>(s.wgi), static_cast<long long>(s.wgj),
        static_cast<unsigned long long>(s.tick));
    return std::string(buf);
}

// Parse + validate the model's JSON into an intent. Bad/missing/unknown → a safe
// default (Hunt, 0.5) with ok=false, never throws (defense in depth).
inline ShoggothIntent parse_shoggoth_intent(const std::string& content, bool& ok) {
    ShoggothIntent intent{};  // default Hunt, 0.5
    ok = false;
    br::director::json::Value v;
    std::string err;
    if (!br::director::json::parse(content, v, err) || !v.is_object()) return intent;
    const auto* a = v.find("action");
    if (!a || !a->is_string()) return intent;
    const std::string& s = a->str;
    if (s == "hunt") intent.action = ShoggothAction::Hunt;
    else if (s == "stalk") intent.action = ShoggothAction::Stalk;
    else if (s == "lurk") intent.action = ShoggothAction::Lurk;
    else if (s == "flank") intent.action = ShoggothAction::Flank;
    else if (s == "flee") intent.action = ShoggothAction::Flee;
    else return intent;  // unknown action -> reject
    const auto* g = v.find("aggression");
    if (g && g->is_number()) {
        const float ag = static_cast<float>(g->num);
        intent.aggression = ag < 0.0f ? 0.0f : (ag > 1.0f ? 1.0f : ag);
    }
    ok = true;
    return intent;
}

// --- the intent event log (record == replay; the model lives only here) ---------
struct ShoggothEvent {
    uint64_t effective_tick;
    int32_t action;
    float aggression;
};

inline bool write_shoggoth_log(const std::string& path, uint64_t seed, uint64_t ticks,
                               const std::vector<ShoggothEvent>& events) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write("SHOGLOG1", 8);
    f.write(reinterpret_cast<const char*>(&seed), 8);
    f.write(reinterpret_cast<const char*>(&ticks), 8);
    uint64_t n = events.size();
    f.write(reinterpret_cast<const char*>(&n), 8);
    for (const auto& e : events) f.write(reinterpret_cast<const char*>(&e), sizeof(ShoggothEvent));
    return static_cast<bool>(f);
}

inline bool read_shoggoth_log(const std::string& path, uint64_t& seed, uint64_t& ticks,
                              std::vector<ShoggothEvent>& events) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[8];
    f.read(magic, 8);
    if (std::memcmp(magic, "SHOGLOG1", 8) != 0) return false;
    f.read(reinterpret_cast<char*>(&seed), 8);
    f.read(reinterpret_cast<char*>(&ticks), 8);
    uint64_t n = 0;
    f.read(reinterpret_cast<char*>(&n), 8);
    if (n > 1000000u) return false;
    events.resize(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) f.read(reinterpret_cast<char*>(&events[i]), sizeof(ShoggothEvent));
    return static_cast<bool>(f);
}

}  // namespace br::app
