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

}  // namespace br::director
