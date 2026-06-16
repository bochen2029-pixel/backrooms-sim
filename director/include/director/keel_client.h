#pragma once
//
// director/keel_client.h — the Director's link to the model: a thin HTTP client for
// the KEEL sidecar's OpenAI-compatible egress (ADR-038). M11. No embedded llama.cpp;
// inference lives in a separate process reached over localhost. Never throws — a
// transport/status/parse failure returns ok=false so the Director degrades to a
// graceful no-op (INV-6), identical to --no-director.
//
#include <cstdint>
#include <string>

namespace br::director {

struct KeelResponse {
    bool        ok = false;
    int         http_status = 0;
    std::string content;     // choices[0].message.content (the directive JSON string)
    std::string tier;        // keel.tier  (expect "local")
    std::string route;       // keel.route
    double      cost = 0.0;  // keel.cost  (expect 0.0)
    std::string error;       // set when !ok
};

// POST `prompt` to http://<host>:<port>/v1/chat/completions, forcing KEEL's local
// tier (sovereign + scaffolding + think:false → $0, no cloud egress, no escalation,
// single-shot). Returns the model content + KEEL routing fields.
KeelResponse keel_complete(const std::string& host, int port, const std::string& prompt,
                           uint32_t timeout_ms = 8000);

// M22 (vision): the same call, but the user turn carries an image alongside the text —
// an OpenAI `image_url` content part with a base64 data URI (`image_base64` is the raw
// base64 of a PNG, no prefix). KEEL routes raw perception to the local vision tier
// (qwen-VL + mmproj, forced local — sovereign, $0). Identical failure semantics: a
// transport/status/parse error returns ok=false (graceful no-op). The vision timeout
// is larger by default since image processing is heavier than text.
KeelResponse keel_complete_vision(const std::string& host, int port, const std::string& prompt,
                                  const std::string& image_base64, uint32_t timeout_ms = 30000);

// Lightweight liveness probe: a short GET to http://<host>:<port>/health. Returns true if anything
// answers (any HTTP status reached), false on connect/timeout failure. Used by the bundle launcher to
// skip a server whose port is already serving (idempotent bring-up). Never throws.
bool service_up(const std::string& host, int port, uint32_t timeout_ms = 800);

}  // namespace br::director
