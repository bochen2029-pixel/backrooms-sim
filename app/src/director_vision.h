#pragma once
//
// app/director_vision.h — the in-game Director's EYES (VLM-grounded narration).
//
// The M11 Director narrated from TEXT STATS only (render_prompt(WandererSummary):
// tick/seed/biome/distance/dwell) -- so it confabulated "fortune-cookie" liminal
// tropes (dripping faucets, dust) ungrounded in the actual screen. This makes the
// Director SEE: the player's rendered POV is handed to the SAME proven Qwen-VL
// pipeline the Shoggoth uses (director::keel_complete_vision + the mmproj), and it
// narrates ONLY what is actually visible in the frame -- the yellow corridor, a
// doorway, a junction, a column, the entity in view.
//
// Two pieces, mirroring the Shoggoth split (shoggoth_vision.h prompt + shoggoth_brain_host.h
// host): a PURE prompt builder, and a header-only async host that runs the (slow) vision
// call OFF the frame thread -- a faithful copy of ShoggothBrainHost's latest-wins submit /
// non-blocking poll / graceful-no-op-when-KEEL-down pattern (the M11/Gate-2 async-isolation
// invariant, INV-6). This is a LIVE PRESENTATION path only: it never touches the sim,
// the replay log, or any golden (INV-1 untouched), exactly like the live Shoggoth brain.
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
#include "keel_broker.h"

namespace br::app {

// The narration prompt: the surveillance-AI framing validated by the de-risk experiment
// (scripts/vision_probe.ps1) -- ONE short, clinical sentence about what is ACTUALLY in the
// attached frame, with a hard anti-hallucination guard. `context` (may be empty) carries
// non-visual sensor data (e.g. a nearby entity) the model MAY reference even if unseen --
// kept separate from the visual-only rule so it grounds sight while still allowing the
// entity-aware beat the operator asked for.
inline std::string render_director_vision_prompt(const std::string& context) {
    std::string p =
        "You are the surveillance intelligence of an endless, abandoned facility, watching a "
        "live feed from a lone wanderer's eyes. The attached image is the current frame from "
        "their camera. Speak ONE short sentence (at most 18 words) to the wanderer over the "
        "intercom about what is ACTUALLY visible in this frame -- the walls, their colour, the "
        "lighting, doorways, openings, turns, columns, or any figure or shape present. Describe "
        "ONLY what you can see in the image; never invent objects that are not there. Tone: calm, "
        "clinical, quietly menacing. Output only the spoken line, with no quotes and no preamble.";
    if (!context.empty()) {
        p += " Facility sensors also report: ";
        p += context;
        p += " You MAY reference this in your line even if it is not visible in the frame.";
    }
    return p;
}

// Tidy the model's reply into a single spoken PA line: drop any stray <think>...</think>,
// collapse whitespace/newlines, strip wrapping quotes, and cap the length (the PA voice +
// subtitle want a short line). Defensive -- think:false already suppresses reasoning.
inline std::string clean_vision_line(const std::string& raw) {
    std::string s = raw;
    // Remove a leading <think>...</think> block if present.
    const size_t ts = s.find("<think>");
    if (ts != std::string::npos) {
        const size_t te = s.find("</think>", ts);
        if (te != std::string::npos) s.erase(ts, te + 8 - ts);
    }
    // Collapse all whitespace runs to single spaces.
    std::string out;
    out.reserve(s.size());
    bool sp = false;
    for (char c : s) {
        const bool ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (ws) { sp = true; continue; }
        if (sp && !out.empty()) out.push_back(' ');
        sp = false;
        out.push_back(c);
    }
    // Strip wrapping quotes the model sometimes adds.
    if (out.size() >= 2 && (out.front() == '"' || out.front() == '\'') && (out.back() == '"' || out.back() == '\''))
        out = out.substr(1, out.size() - 2);
    if (out.size() > 220) out = out.substr(0, 220);
    return out;
}

// A background-thread Qwen-VL narrator. Construct once; `submit` a base64 PNG of the
// player's POV (+ optional sensor context) on an ambient cadence and drain `poll` every
// frame. Destruction joins the worker. Reuses director::keel_complete_vision verbatim --
// the exact pipeline the M22 Shoggoth proved -- so a KEEL/VLM outage is a graceful no-op.
class DirectorVisionHost {
public:
    DirectorVisionHost(std::string host, int port, uint32_t timeout_ms = 30000, KeelBroker* broker = nullptr)
        : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms), broker_(broker) {
        thread_ = std::thread(&DirectorVisionHost::worker_loop, this);
    }
    ~DirectorVisionHost() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    DirectorVisionHost(const DirectorVisionHost&) = delete;
    DirectorVisionHost& operator=(const DirectorVisionHost&) = delete;

    // Non-blocking. Latest-wins: a newer POV supersedes a queued one, so at most one
    // vision call is ever in flight and the narration always describes the present view.
    void submit(std::string image_b64, std::string context) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pending_b64_ = std::move(image_b64);
            pending_ctx_ = std::move(context);
            have_pending_ = true;
        }
        cv_.notify_one();
    }

    // Non-blocking. Hands back any narration lines produced since the last poll.
    std::vector<std::string> poll() {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lk(mtx_);
        out.swap(ready_);
        return out;
    }

    uint64_t requests() const { return requests_.load(); }   // vision calls attempted
    uint64_t produced() const { return produced_.load(); }   // non-empty lines yielded

private:
    void worker_loop() {
        for (;;) {
            std::string b64, ctx;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [&] { return stop_ || have_pending_; });
                if (stop_) return;
                b64 = std::move(pending_b64_);
                ctx = std::move(pending_ctx_);
                have_pending_ = false;
            }
            requests_.fetch_add(1);
            // The blocking VLM call runs OUTSIDE the lock, on this worker -- never the
            // frame thread. KEEL down / error -> ok=false -> a graceful no-op.
            // Phase C.2: gated by the shared KeelBroker -- this multimodal narration competes for the single
            // vision slot below shoggoth-vision and player-speech. broker drop / KEEL down -> ok=false.
            const br::director::KeelResponse resp = broker_gated<br::director::KeelResponse>(
                broker_, static_cast<int>(KeelPriority::DirectorVision),
                static_cast<uint32_t>(KeelPriority::DirectorVision), /*multimodal=*/true,
                [&] { return br::director::keel_complete_vision(host_, port_, render_director_vision_prompt(ctx), b64, timeout_ms_); });
            if (!resp.ok) continue;
            const std::string line = clean_vision_line(resp.content);
            if (line.empty()) continue;
            produced_.fetch_add(1);
            std::lock_guard<std::mutex> lk(mtx_);
            ready_.push_back(line);
        }
    }

    std::string host_;
    int port_;
    uint32_t timeout_ms_;
    KeelBroker* broker_ = nullptr;   // Phase C.2: shared arbiter (nullptr -> legacy direct call)
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool have_pending_ = false;
    std::string pending_b64_{};
    std::string pending_ctx_{};
    std::vector<std::string> ready_;
    std::atomic<uint64_t> requests_{0};
    std::atomic<uint64_t> produced_{0};
};

}  // namespace br::app
