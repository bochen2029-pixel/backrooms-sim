#pragma once
//
// contracts/director_v1.h — sim <-> director boundary (v1.0).
//
// The Director is an ambient game-master: it reads a deterministic WandererSummary
// and emits a Directive. In M11 directives are **presentation-layer only** — they
// drive rendering / audio / HUD atmosphere through a RECORDED event stream and
// NEVER perturb the WorldState hash or chunk generation, so INV-1 (sim determinism)
// and INV-2 (generation purity) stay provably intact. Directives reach the sim only
// as validated Event-Log entries scheduled at a deterministic effective_tick
// (INV-5); raw model text never crosses this boundary. Replays consume the recorded
// log, never the model — so a replay is bit-identical with the model fully offline.
//
#include <cstdint>

namespace br::contracts {

// What the sim hands the Director each cycle — derived deterministically from the
// already-hashed WorldState at `tick`, so the same run produces the same summaries.
struct WandererSummary {
    uint64_t tick;
    uint64_t world_seed;
    int32_t  level;
    int64_t  chunk_cx, chunk_cz;
    int32_t  biome;          // current biome id (gen::Biome value, 0..kDirectorBiomeCount-1)
    float    distance_m;     // odometer
    float    dwell_seconds;  // time lingering near the current location
    uint32_t route_loops;    // detected path loops (revisits of a recent cell)
    uint64_t location_hash;  // stable hash of the spot (Wanderer-Note cache key)
};

enum class DirectiveKind : int32_t {
    None          = 0,
    FlickerSector = 1,   // flicker a sector of fluorescent lights (render)
    SoundCue      = 2,   // a distant sound cue (audio)
    BiomeBias     = 3,   // an atmospheric biome lean (surfaced as a cue; no gen change in M11)
    Intercom      = 4,   // a Voice / intercom caption (HUD)
    WandererNote  = 5,   // a note left at a location, cached per location_hash (HUD)
};

// Bounds the validator enforces (lint). Out-of-range structured fields are
// REJECTED, never clamped — an invalid directive is logged and dropped whole.
constexpr int32_t kDirectorMaxSector   = 64;    // FlickerSector: sector in [0, kDirectorMaxSector)
constexpr int32_t kDirectorBiomeCount  = 5;     // BiomeBias: biome in [0, kDirectorBiomeCount)
constexpr int     kDirectiveCaptionCap = 128;   // caption buffer bytes (incl. NUL)

// A validated directive. POD + fixed-size so it serializes verbatim into the replay
// record. Fields not used by a given kind are zero / empty.
struct Directive {
    DirectiveKind kind = DirectiveKind::None;
    int32_t sector = 0;            // FlickerSector
    int32_t biome = 0;            // BiomeBias
    float   intensity = 0.0f;     // 0..1 (flicker depth / cue loudness)
    char    caption[kDirectiveCaptionCap] = {};  // Intercom / WandererNote / cue text; printable ASCII, NUL-terminated
};

// A directive scheduled into the recorded event stream at a deterministic tick.
struct DirectorEvent {
    uint64_t  effective_tick = 0;  // sim tick at which the directive becomes active
    Directive directive;
};

}  // namespace br::contracts
