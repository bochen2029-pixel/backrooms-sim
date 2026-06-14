#pragma once
//
// director/host.h — Director orchestration (M11): the synchronous request path and
// the Director Event Log I/O. A live run RECORDS validated directives here; a replay
// consumes the log with the model fully offline and reproduces the run bit-identically
// (INV-1) — the model's output reaches the sim only as these logged events.
//
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "contracts/director_v1.h"

namespace br::director {

// Synchronously request one directive from KEEL: render_prompt → keel_complete →
// validate_directive. Returns the validated Directive, or nullopt if KEEL is
// unreachable / the response is off-schema (rejected + dropped). Blocking; used by
// the determinism record path and (wrapped on a thread) by the async host.
std::optional<contracts::Directive> request_directive(const std::string& host, int port,
                                                      const contracts::WandererSummary& summary,
                                                      uint32_t timeout_ms = 15000);

// Director Event Log I/O (replay_v1 director-log format). `write` returns false on
// I/O error; `read` returns false on I/O error or bad magic/version, otherwise fills
// `world_seed` + `run_ticks` + `events`.
bool write_director_log(const std::string& path, uint64_t world_seed, uint64_t run_ticks,
                        const std::vector<contracts::DirectorEvent>& events);
bool read_director_log(const std::string& path, uint64_t& world_seed, uint64_t& run_ticks,
                       std::vector<contracts::DirectorEvent>& events);

}  // namespace br::director
