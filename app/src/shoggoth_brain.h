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
    // Models (especially the vision tier) sometimes wrap the JSON in a markdown ```json
    // fence or surround it with prose; extract the outermost {...} span so a fenced or
    // chatty reply still parses. Bare JSON is unchanged; no braces -> the safe default.
    const size_t b = content.find('{');
    const size_t e = content.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e < b) return intent;
    const std::string json_str = content.substr(b, e - b + 1);
    br::director::json::Value v;
    std::string err;
    if (!br::director::json::parse(json_str, v, err) || !v.is_object()) return intent;
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
    // Phase D/E: the semantic perception fields + the voice utterance. All OPTIONAL -- the text-brain
    // reply (action/aggression only) leaves them at their defaults, so its behaviour + the sacred gate
    // are unchanged; the vision reply fills them and drives resolve_target + the voice.
    if (const auto* tk = v.find("target_kind"); tk && tk->is_string()) {
        const std::string& k = tk->str;
        if (k == "wanderer") intent.target_kind = TargetKind::Wanderer;
        else if (k == "doorway") intent.target_kind = TargetKind::Doorway;
        else if (k == "stairs") intent.target_kind = TargetKind::Stairs;
        else if (k == "shaft") intent.target_kind = TargetKind::Shaft;
        else if (k == "dark") intent.target_kind = TargetKind::Dark;
        else if (k == "light") intent.target_kind = TargetKind::Light;
    }
    if (const auto* se = v.find("sector"); se && se->is_string()) {
        const std::string& k = se->str;
        if (k == "ahead") intent.sector = Sector::Ahead;
        else if (k == "ahead_left") intent.sector = Sector::AheadLeft;
        else if (k == "left") intent.sector = Sector::Left;
        else if (k == "behind_left") intent.sector = Sector::BehindLeft;
        else if (k == "behind") intent.sector = Sector::Behind;
        else if (k == "behind_right") intent.sector = Sector::BehindRight;
        else if (k == "right") intent.sector = Sector::Right;
        else if (k == "ahead_right") intent.sector = Sector::AheadRight;
    }
    if (const auto* px = v.find("proximity"); px && px->is_string()) {
        const std::string& k = px->str;
        if (k == "near") intent.proximity = Proximity::Near;
        else if (k == "mid") intent.proximity = Proximity::Mid;
        else if (k == "far") intent.proximity = Proximity::Far;
    }
    if (const auto* md = v.find("mood"); md && md->is_string()) {
        const std::string& k = md->str;
        if (k == "curious") intent.mood = Mood::Curious;
        else if (k == "fixated") intent.mood = Mood::Fixated;
        else if (k == "afraid") intent.mood = Mood::Afraid;
        else if (k == "idle") intent.mood = Mood::Idle;
    }
    if (const auto* sn = v.find("snap"); sn && sn->is_number()) {
        const float q = static_cast<float>(sn->num);
        intent.snap = q < 0.0f ? 0.0f : (q > 1.0f ? 1.0f : q);
    }
    if (const auto* ut = v.find("utterance"); ut && ut->is_string()) {
        intent.utterance = ut->str;
        if (intent.utterance.size() > 120u) intent.utterance.resize(120u);  // structural cage: hard length cap
    }
    ok = true;
    return intent;
}

// --- the intent event log (record == replay; the model lives only here) ---------
struct ShoggothEvent {
    uint64_t effective_tick;   // 8
    int32_t action;            // 4
    float aggression;          // 4
    // Phase B (SHOGLOG2) motion fields. int32_t-backed enums + snap. Laid out PADDING-FREE so the
    // raw-byte fold (`fold_bytes` over sizeof) stays deterministic across runs; the static_assert
    // locks the invariant at compile time (a non-LLM oracle). `_reserved` pads to a multiple of 8
    // AND leaves room for one more field later without another version bump.
    int32_t target_kind;       // 4
    int32_t sector;            // 4
    int32_t proximity;         // 4
    int32_t mood;              // 4
    float snap;                // 4
    int32_t _reserved;         // 4  -> 40 total
};
static_assert(sizeof(ShoggothEvent) == 40,
              "ShoggothEvent must stay padding-free (40 bytes) for the deterministic raw-byte fold");

// Phase B: the SINGLE SOURCE OF TRUTH for the intent<->event mapping. Every record path builds its
// event via event_from_intent; replay reconstructs the intent via apply_event_to_intent. Adding a
// motion field later is one edit in each helper, not N across the codebase. Only MOTION fields
// cross the boundary (the voice utterance is presentation-only and never serialized).
inline ShoggothEvent event_from_intent(uint64_t tick, const ShoggothIntent& it) {
    ShoggothEvent e{};  // zero-init (defensive; the layout is padding-free regardless)
    e.effective_tick = tick;
    e.action = static_cast<int32_t>(it.action);
    e.aggression = it.aggression;
    e.target_kind = static_cast<int32_t>(it.target_kind);
    e.sector = static_cast<int32_t>(it.sector);
    e.proximity = static_cast<int32_t>(it.proximity);
    e.mood = static_cast<int32_t>(it.mood);
    e.snap = it.snap;
    return e;
}
inline void apply_event_to_intent(const ShoggothEvent& e, ShoggothIntent& it) {
    it.action = static_cast<ShoggothAction>(e.action);
    it.aggression = e.aggression;
    it.target_kind = static_cast<TargetKind>(e.target_kind);
    it.sector = static_cast<Sector>(e.sector);
    it.proximity = static_cast<Proximity>(e.proximity);
    it.mood = static_cast<Mood>(e.mood);
    it.snap = e.snap;
}

inline bool write_shoggoth_log(const std::string& path, uint64_t seed, uint64_t ticks,
                               const std::vector<ShoggothEvent>& events) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write("SHOGLOG2", 8);
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
    if (std::memcmp(magic, "SHOGLOG2", 8) != 0) return false;
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
