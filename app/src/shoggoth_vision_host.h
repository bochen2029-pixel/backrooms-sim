#pragma once
//
// app/shoggoth_vision_host.h — the Shoggoth's LIVE async EYES (Phase D LIVE).
//
// shoggoth_vision.h gave the creature a PURE POV camera + vision prompt, and the
// headless --shoggoth-vision-record path proved that a rendered POV -> qwen-VL ->
// validated intent enters the deterministic creature bit-exactly (the sacred gate,
// now with eyes). But in the actual PLAYABLE game the creature still reasoned from
// its TEXT sense alone (ShoggothBrainHost). This host closes that gap: it runs the
// (slow) vision call OFF the frame thread, turning a rendered POV snapshot + a
// ShoggothSummary into a validated ShoggothIntent — the SAME latest-wins submit /
// non-blocking poll / graceful-no-op-when-KEEL-down shape as DirectorVisionHost and
// ShoggothBrainHost (the M11/Gate-2 async-isolation invariant, INV-6).
//
// LIVE PRESENTATION path only (run_game): the returned intent nudges sh.intent at a
// tick boundary exactly like the live text brain — it never touches the sim core, the
// replay log, or any golden (INV-1 untouched). The intent's target_kind drives
// resolve_target, so the creature's MOTION follows what it actually SEES.
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
#include "shoggoth_vision.h"

namespace br::app {

// A background-thread qwen-VL eye for the Shoggoth. Construct once; `submit` a base64
// PNG of the creature's POV (+ its situational ShoggothSummary) on a sparse cadence and
// drain `poll` every frame. Destruction joins the worker. Reuses the exact vision client +
// prompt + intent validator the M22 record path proved — so a KEEL/VLM outage is a graceful
// no-op (no intent yielded, the live text brain keeps driving).
class ShoggothVisionHost {
public:
    ShoggothVisionHost(std::string host, int port, uint32_t timeout_ms = 30000)
        : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {
        thread_ = std::thread(&ShoggothVisionHost::worker_loop, this);
    }
    ~ShoggothVisionHost() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    ShoggothVisionHost(const ShoggothVisionHost&) = delete;
    ShoggothVisionHost& operator=(const ShoggothVisionHost&) = delete;

    // Non-blocking. Latest-wins: a newer POV supersedes a queued one, so at most one
    // vision call is ever in flight and the intent always reflects the present view.
    void submit(std::string image_b64, const ShoggothSummary& summary) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pending_b64_ = std::move(image_b64);
            pending_sum_ = summary;
            have_pending_ = true;
        }
        cv_.notify_one();
    }

    // Non-blocking. Hands back any validated intents produced since the last poll
    // (move-swap, so the internal slot clears). Empty when nothing is ready / KEEL is down.
    std::vector<ShoggothIntent> poll() {
        std::vector<ShoggothIntent> out;
        std::lock_guard<std::mutex> lk(mtx_);
        out.swap(ready_);
        return out;
    }

    uint64_t requests() const { return requests_.load(); }   // vision calls attempted
    uint64_t produced() const { return produced_.load(); }   // valid intents yielded

private:
    void worker_loop() {
        for (;;) {
            std::string b64;
            ShoggothSummary sum;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [&] { return stop_ || have_pending_; });
                if (stop_) return;
                b64 = std::move(pending_b64_);
                sum = pending_sum_;
                have_pending_ = false;
            }
            requests_.fetch_add(1);
            // The blocking VLM call runs OUTSIDE the lock, on this worker — never the
            // frame thread. KEEL down / error -> ok=false -> a graceful no-op.
            const br::director::KeelResponse resp =
                br::director::keel_complete_vision(host_, port_, render_shoggoth_vision_prompt(sum), b64, timeout_ms_);
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
    std::string pending_b64_{};
    ShoggothSummary pending_sum_{};
    std::vector<ShoggothIntent> ready_;
    std::atomic<uint64_t> requests_{0};
    std::atomic<uint64_t> produced_{0};
};

}  // namespace br::app
