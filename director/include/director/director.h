#pragma once
// director/director.h — llama.cpp host, WandererSummary -> Directive pipeline,
// schema validation, note cache, Event Log emission. M0 stub: identity only.
// Real LLM host arrives M11. Raw model text never crosses into the sim (INV-5);
// directives enter only as validated Event Log entries.
namespace br::director {
const char* module_name() noexcept;
}  // namespace br::director
