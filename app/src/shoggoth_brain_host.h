#pragma once
//
// app/shoggoth_brain_host.h — the Shoggoth's LIVE async brain (M21b).
//
// M21 gave the creature a KEEL brain, but only in the headless --shoggoth-record /
// --shoggoth-replay modes (which prove the sacred determinism gate). In the actual
// playable game (--game / --play) the monster ran with the default `Hunt` intent — it
// never thought live. This host makes it think WHILE you play, WITHOUT ever hitching
// the 120 Hz frame loop: a background thread turns a `ShoggothSummary` into a KEEL
// inference + a validated `ShoggothIntent` OFF the frame thread, handing the result
// back via a mutex-guarded slot + atomics.
//
// It is a faithful mirror of the Director's async `DirectorHost` (director/host.h):
// non-blocking latest-wins `submit`, non-blocking `poll`, graceful no-op when KEEL is
// down (the M11/Gate-2 "async isolation" pattern). The intent it returns is applied to
// the live Shoggoth as a discrete event at a tick boundary — the same "intent enters
// as a timestamped event" shape the headless record/replay sacred gate proves bit-exact
// (the LLM never couples to the sim continuously; it only ever nudges `sh.intent`).
//
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "director/keel_client.h"
#include "shoggoth_brain.h"

namespace br::app {

// A background-thread KEEL brain for the Shoggoth. Construct it once; `submit` a
// summary on an ambient cadence and drain `poll` every frame. Destruction joins the
// worker. Reuses the exact KEEL client + prompt + validator the M21 brain uses.
class ShoggothBrainHost {
public:
    ShoggothBrainHost(std::string host, int port, uint32_t timeout_ms = 8000)
        : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {
        thread_ = std::thread(&ShoggothBrainHost::worker_loop, this);
    }
    ~ShoggothBrainHost() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    ShoggothBrainHost(const ShoggothBrainHost&) = delete;
    ShoggothBrainHost& operator=(const ShoggothBrainHost&) = delete;

    // Non-blocking. Latest-wins: a newer summary supersedes a queued one, so at most
    // one inference is ever in flight and the brain always reasons about the present.
    void submit(const ShoggothSummary& summary) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pending_ = summary;
            have_pending_ = true;
        }
        cv_.notify_one();
    }

    // Non-blocking. Hands back any validated intents produced since the last poll
    // (move-swap, so the internal slot is cleared). Empty when nothing is ready or
    // KEEL is unreachable.
    std::vector<ShoggothIntent> poll() {
        std::vector<ShoggothIntent> out;
        std::lock_guard<std::mutex> lk(mtx_);
        out.swap(ready_);
        return out;
    }

    uint64_t requests() const { return requests_.load(); }   // KEEL calls attempted
    uint64_t produced() const { return produced_.load(); }   // valid intents yielded

private:
    void worker_loop() {
        for (;;) {
            ShoggothSummary s;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [&] { return stop_ || have_pending_; });
                if (stop_) return;
                s = pending_;
                have_pending_ = false;
            }
            requests_.fetch_add(1);
            // The blocking KEEL call runs OUTSIDE the lock, on this worker — never the
            // frame thread. KEEL down / error -> ok=false -> a graceful no-op.
            const br::director::KeelResponse resp =
                br::director::keel_complete(host_, port_, render_shoggoth_prompt(s), timeout_ms_);
            if (!resp.ok) continue;
            bool ok = false;
            const ShoggothIntent intent = parse_shoggoth_intent(resp.content, ok);
            if (!ok) continue;  // invalid model output -> drop (defense in depth)
            produced_.fetch_add(1);
            std::lock_guard<std::mutex> lk(mtx_);
            ready_.push_back(intent);
        }
    }

    std::string host_;
    int port_;
    uint32_t timeout_ms_;
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool have_pending_ = false;
    ShoggothSummary pending_{};
    std::vector<ShoggothIntent> ready_;
    std::atomic<uint64_t> requests_{0};
    std::atomic<uint64_t> produced_{0};
};

}  // namespace br::app
