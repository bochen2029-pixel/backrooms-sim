#pragma once
//
// app/shoggoth_hearing.h — the Shoggoth's EARS (M23): a heard-sound prompt (PURE).
//
// M22 gave the creature sight; M23 gives it hearing. At record time the app renders a
// short clip of the Backrooms soundscape at the Shoggoth's vantage (the fluorescent hum
// + the wanderer's footfalls, louder the nearer it is), runs whisper.cpp over it, and
// feeds the resulting transcript into the brain. The Backrooms has no speech, so whisper
// returns coarse sound-event tags ("(upbeat music)", "(silence)", "(footsteps)") rather
// than words — a deliberately coarse sense (ADR-050). These two pieces are PURE (no
// process, no audio): cleaning the transcript and building the prompt. The whisper call
// + the audio render live in the app's record path and, exactly like vision, happen at
// RECORD time only — the resulting intent enters via the event log, so a replay with
// whisper AND the model offline is bit-identical.
//
#include <cstdio>
#include <string>

#include "shoggoth_brain.h"

namespace br::app {

// Trim leading/trailing whitespace + newlines from a whisper transcript (the -otxt file
// is usually " (upbeat music)\n"). An empty result means "heard nothing of note".
inline std::string clean_transcript(const std::string& raw) {
    size_t b = 0, e = raw.size();
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (b < e && is_ws(raw[b])) ++b;
    while (e > b && is_ws(raw[e - 1])) --e;
    return raw.substr(b, e - b);
}

// The hearing prompt: the creature is told what its ears picked up (a sound tag) plus its
// situational sense, then must emit exactly one intent JSON — the same schema
// parse_shoggoth_intent validates, so a bad reply still degrades to Hunt.
inline std::string render_shoggoth_hearing_prompt(const ShoggothSummary& s, const std::string& heard) {
    static const char* kStates[4] = {"lurking", "hunting", "chasing", "retreating"};
    const std::string ear = heard.empty() ? std::string("silence") : heard;
    char buf[1400];
    std::snprintf(buf, sizeof(buf),
        "You are a SHOGGOTH - a vast, amorphous, intelligent horror loose in the infinite "
        "Backrooms, hunting a lone wanderer. Your formless body HEARS the space around you; "
        "a transcription of the sound right now is: \"%s\". Footsteps mean the wanderer is "
        "near and moving; near-silence or only the fluorescent drone means it is far or "
        "still. Combine what you HEAR with what you sense: you are %s, %.0f m from the "
        "wanderer; you are at cell (%lld,%lld) and it is at (%lld,%lld). Choose ONE "
        "behaviour and emit EXACTLY ONE compact JSON object and nothing else:\n"
        "{\"action\":\"hunt|stalk|lurk|flank|flee\",\"aggression\":<0.0-1.0>}\n"
        "  hunt = close in directly;  stalk = creep closer slowly;  lurk = wait / withdraw;\n"
        "  flank = circle around to cut it off;  flee = retreat (rare). Output ONLY the JSON.",
        ear.c_str(), kStates[(s.state >= 0 && s.state < 4) ? s.state : 0],
        static_cast<double>(s.distance_m),
        static_cast<long long>(s.sgi), static_cast<long long>(s.sgj),
        static_cast<long long>(s.wgi), static_cast<long long>(s.wgj));
    return std::string(buf);
}

}  // namespace br::app
