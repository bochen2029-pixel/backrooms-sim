#pragma once
//
// director/director.h — the Director (M11): WandererSummary -> KEEL -> Directive,
// schema-validated. Raw model text never crosses into the sim (INV-5); directives
// enter only as validated, logged Event-Log entries (contracts/director_v1.h). In
// M11 the model is reached over HTTP via the KEEL sidecar (OpenAI-compatible egress,
// ADR-038), not an embedded llama.cpp — so the sim's only third-party deps stay
// Catch2 + stb, and generation runs in a separate process (cleaner isolation).
//
#include <string>

#include "contracts/director_v1.h"

namespace br::director {

const char* module_name() noexcept;

// Outcome of validating a model `content` string against the Directive schema.
struct DirectiveResult {
    bool ok = false;
    contracts::Directive directive{};
    std::string reject_reason;   // set when !ok; logged, never applied (defense-in-depth)
};

// Parse + validate one directive JSON object (the model's message content) against
// the M11 schema:
//   {"type":"flicker","sector":0..63,"intensity":0..1[,"detail":"..."]}
//   {"type":"sound","intensity":0..1[,"detail":"..."]}
//   {"type":"biome_bias","biome":0..4[,"detail":"..."]}
//   {"type":"intercom"|"note","detail":"<non-empty>"}
// Rejects malformed JSON, unknown/missing type, out-of-range or missing structured
// fields; sanitises captions to bounded printable ASCII. Pure + total (no I/O).
DirectiveResult validate_directive(const std::string& json_content);

// Render a WandererSummary into the Director's instruction prompt (the exact
// directive schema + the situation). Record-time only; never touches the sim.
std::string render_prompt(const contracts::WandererSummary& s);

}  // namespace br::director
