#pragma once
//
// director/host.h — Director orchestration (M11): the synchronous request path and
// the Director Event Log I/O. A live run RECORDS validated directives here; a replay
// consumes the log with the model fully offline and reproduces the run bit-identically
// (INV-1) — the model's output reaches the sim only as these logged events.
//
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

// Async Director host: runs request_directive on its own worker thread so model
// generation never touches the sim/frame thread (the async-isolation invariant). The
// caller submits the latest WandererSummary (non-blocking, latest-wins; at most one
// request in flight) and polls completed directives. Generation latency stays off
// the hot path entirely.
class DirectorHost {
public:
    DirectorHost(std::string host, int port, uint32_t timeout_ms = 8000);
    ~DirectorHost();
    DirectorHost(const DirectorHost&) = delete;
    DirectorHost& operator=(const DirectorHost&) = delete;

    void submit(const contracts::WandererSummary& summary);   // non-blocking
    std::vector<contracts::Directive> poll();                 // drain ready (non-blocking)
    uint64_t requests() const { return requests_.load(); }    // KEEL calls made
    uint64_t produced() const { return produced_.load(); }    // valid directives produced

private:
    void worker_loop();

    std::string host_;
    int port_;
    uint32_t timeout_ms_;
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool have_pending_ = false;
    contracts::WandererSummary pending_{};
    std::vector<contracts::Directive> ready_;
    std::atomic<uint64_t> requests_{0};
    std::atomic<uint64_t> produced_{0};
};

}  // namespace br::director
