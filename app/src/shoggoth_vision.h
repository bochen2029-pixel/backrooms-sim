#pragma once
//
// app/shoggoth_vision.h — the Shoggoth's EYES (M22): a POV camera + a vision prompt.
//
// The creature's brain (M21) reasoned from text alone (cells + distance). M22 gives it
// sight: a virtual camera at the Shoggoth's vantage renders an offscreen snapshot of the
// Backrooms ahead of it, which a vision model (qwen-VL + mmproj, via KEEL's local tier)
// looks at when choosing its intent. These two pieces are PURE (no renderer, no network):
// the camera pose and the prompt. The actual offscreen render + the KEEL vision call live
// in the app's record path, and — exactly like M21 — happen at RECORD time only, with the
// resulting intent entering the deterministic shoggoth as an event-log entry, so a replay
// with the model offline (and the snapshot never re-rendered) is bit-identical.
//
#include <cstdio>
#include <string>

#include "contracts/world_view_v1.h"
#include "shoggoth.h"
#include "shoggoth_brain.h"

namespace br::app {

// The camera the Shoggoth sees through: at its body, an eye-height above the floor,
// facing where it is heading (its yaw — toward the wanderer when hunting), tilted a hair
// down so the corridor floor and walls fill the frame.
inline contracts::CameraPose shoggoth_pov_camera(const Shoggoth& sh, float aspect) {
    contracts::CameraPose cam{};
    cam.pos[0] = sh.pos.x;
    cam.pos[1] = sh.pos.y + br::core::kEyeHeight;
    cam.pos[2] = sh.pos.z;
    cam.yaw = sh.yaw;
    cam.pitch = -0.08f;          // a slight downward tilt, like the wanderer's view
    cam.fov_y = 1.2217305f;      // 70 degrees, matching the rest of the sim
    cam.aspect = aspect;
    return cam;
}

// The vision prompt: the creature is shown the attached image of what it sees AND given
// its situational sense (distance + cells), then must emit exactly one intent JSON — the
// same schema parse_shoggoth_intent validates, so a bad reply still degrades to Hunt.
inline std::string render_shoggoth_vision_prompt(const ShoggothSummary& s) {
    static const char* kStates[4] = {"lurking", "hunting", "chasing", "retreating"};
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        "You are a SHOGGOTH - a vast, amorphous, dim, patient horror loose in the infinite Backrooms, "
        "hunting a lone wanderer. Your single eye SEES the attached image: the corridor ahead (yellowed "
        "walls, fluorescent buzz, openings, junctions, dead ends). What in the frame draws you? Combine "
        "what you SEE with what you sense: you are %s, %.0f m from the wanderer; you are at cell "
        "(%lld,%lld) and it is at (%lld,%lld). Emit EXACTLY ONE compact JSON object and nothing else:\n"
        "{\"action\":\"hunt|stalk|lurk|flank|flee\",\"aggression\":<0.0-1.0>,"
        "\"target_kind\":\"wanderer|doorway|stairs|shaft|dark|light|none\","
        "\"sector\":\"ahead|ahead_left|left|right|ahead_right|behind\",\"proximity\":\"near|mid|far\","
        "\"mood\":\"curious|fixated|afraid|idle\",\"utterance\":\"\"}\n"
        "  target_kind = what in the image pulls you (a doorway? the dark? the wanderer?); sector + "
        "proximity = where it sits in your view. utterance = at most a dozen words you MURMUR if it is "
        "near - impressionistic, sensory, NEVER naming objects (e.g. \"something soft... closer\"), or "
        "\"\" for silence. action: hunt=close in; stalk=creep; lurk=wait; flank=circle; flee=retreat. "
        "Output ONLY the JSON.",
        kStates[(s.state >= 0 && s.state < 4) ? s.state : 0], static_cast<double>(s.distance_m),
        static_cast<long long>(s.sgi), static_cast<long long>(s.sgj),
        static_cast<long long>(s.wgi), static_cast<long long>(s.wgj));
    return std::string(buf);
}

}  // namespace br::app
