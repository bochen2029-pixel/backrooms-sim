#pragma once
//
// app/keel_broker.h — Phase C.2 (SHOGGOTH_PLAN.md §5/§7): the THREADED arbiter wrapping the pure
// KeelScheduler.
//
// The single llama-server backend is shared by the live consumers (player-speech, shoggoth-vision,
// director-vision, shoggoth-brain). Each runs on its own host worker thread and, instead of calling
// keel_complete* directly, brackets the call with acquire()/release(): acquire() registers the request
// (latest-wins per class) and BLOCKS until the scheduler admits it — enforcing the priority order, the
// single multimodal (GPU/VRAM) slot, and the concurrency cap ACROSS all hosts. The admitted call then
// runs on the caller's own thread (no dispatcher thread, no result routing); the host keeps its existing
// submit/poll/latest-wins unchanged. This is the design the KeelScheduler header anticipates: "the
// threaded KeelBroker wraps it with a mutex + condition_variable and runs each admitted request's thunk
// on the caller's host thread."
//
// LIVE PRESENTATION path only (run_game). The broker changes WHEN calls happen, never WHAT enters the
// deterministic sim — the validated results still flow through each host's poll exactly as before, and
// the record/replay path does not use these hosts. So INV-1 / INV-4 (determinism) are untouched.
//
// All decision logic lives in the pure, thread-free KeelScheduler (unit-tested without threads, zero
// flakiness). This file adds ONLY the threading: a mutex + condvar around it, plus a graceful shutdown
// that wakes every blocked acquire() so host workers can join.
//
#include <condition_variable>
#include <cstddef>
#include <mutex>

#include "keel_scheduler.h"

namespace br::app {

class KeelBroker {
public:
    enum class Grant { Admitted, Superseded, Shutdown };

    explicit KeelBroker(int max_concurrency = 2) : sched_(max_concurrency) {}
    ~KeelBroker() { shutdown(); }
    KeelBroker(const KeelBroker&) = delete;
    KeelBroker& operator=(const KeelBroker&) = delete;

    // Called from a host worker thread. Registers a request (latest-wins per class_id: a newer same-class
    // add supersedes any still-waiting older one) and BLOCKS until exactly one of:
    //   - the scheduler admits it           -> Grant::Admitted (run the call, then you MUST release(out_ticket));
    //   - a newer same-class add supersedes  -> Grant::Superseded (drop the work; do NOT release);
    //   - the broker is shutting down        -> Grant::Shutdown  (exit the worker; do NOT release).
    // While blocked the worker holds no lock (cv_.wait releases it), so other hosts proceed freely.
    Grant acquire(KeelTicket& out_ticket, int priority, uint32_t class_id, bool multimodal) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (stop_) { out_ticket = 0; return Grant::Shutdown; }
        const KeelTicket t = sched_.add(priority, class_id, multimodal);
        out_ticket = t;
        cv_.notify_all();   // a sibling we just superseded must wake to learn it was dropped
        for (;;) {
            if (stop_) { sched_.drop(t); return Grant::Shutdown; }
            if (sched_.is_superseded(t)) { sched_.drop(t); return Grant::Superseded; }
            if (sched_.try_admit(t)) return Grant::Admitted;   // admits ONLY us, iff we are the best admissible
            cv_.wait(lk);                                      // a release / supersede / shutdown will re-wake us
        }
    }

    // Called by the host after an Admitted call completes (success OR failure). Frees the concurrency +
    // multimodal slot and wakes the waiters so the next-highest-priority request can be admitted.
    void release(KeelTicket ticket) {
        { std::lock_guard<std::mutex> lk(mtx_); sched_.finish(ticket); }
        cv_.notify_all();
    }

    // Wake every blocked acquire() with Grant::Shutdown so host workers can return + join. Idempotent;
    // call it explicitly BEFORE tearing down the hosts (so no worker is stranded in acquire()), and the
    // dtor calls it again as a backstop. The broker must outlive the hosts (declare it before them).
    void shutdown() {
        { std::lock_guard<std::mutex> lk(mtx_); stop_ = true; }
        cv_.notify_all();
    }

    // Diagnostics (lock-guarded snapshots).
    std::size_t pending() { std::lock_guard<std::mutex> lk(mtx_); return sched_.pending(); }
    int running_count() { std::lock_guard<std::mutex> lk(mtx_); return sched_.running_count(); }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    KeelScheduler sched_;
    bool stop_ = false;
};

}  // namespace br::app
