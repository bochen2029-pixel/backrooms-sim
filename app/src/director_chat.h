#pragma once
//
// app/director_chat.h — the TWO-WAY Director (ADR-074): the wanderer speaks, the facility answers.
//
// The output half (PA voice) and the eyes (director_vision.h) already exist. This is the
// CONVERSATION: a captured spoken utterance (mic_capture.h -> a WAV) is transcribed by
// whisper, then -- with the player's current POV attached -- sent to Qwen-VL with an
// in-character "surveillance intelligence" prompt, and the reply is spoken back through the
// PA voice. An off-thread host (a mirror of DirectorVisionHost) keeps the whisper + VLM
// round-trip (~6-10 s) entirely off the frame thread.
//
// Whisper lives in the app TU (it shells out to whisper-cli); the host takes it as an
// injected functor so this header stays free of that dependency. Live presentation only --
// the conversation never touches the sim/replay/goldens (INV-1), like the live brain.
//
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "director/keel_client.h"
#include "keel_broker.h"
#include "director_vision.h"   // clean_vision_line

namespace br::app {

// Is the transcript real speech worth answering? Whisper on silence/noise emits "[BLANK_AUDIO]",
// "(wind blowing)", bare punctuation, or its classic hallucinations ("Thank you", "you"). Strip
// bracketed/parenthetical tags, require a few real letters and >=2 tokens-ish, and reject the
// known junk. Imperfect (VAD's tradeoff) but it keeps the Director from answering the room tone.
inline bool plausible_utterance(const std::string& t) {
    std::string s;
    int depth = 0;
    for (char c : t) {
        if (c == '[' || c == '(') ++depth;
        else if (c == ']' || c == ')') { if (depth > 0) --depth; }
        else if (depth == 0) s.push_back(c);
    }
    size_t a = s.find_first_not_of(" \t\r\n.,!?-");
    if (a == std::string::npos) return false;
    size_t b = s.find_last_not_of(" \t\r\n.,!?-");
    s = s.substr(a, b - a + 1);
    int alpha = 0;
    for (char c : s) if (std::isalpha(static_cast<unsigned char>(c))) ++alpha;
    if (alpha < 4) return false;
    std::string low;
    for (char c : s) low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (low == "you" || low == "thank you" || low == "thanks" || low == "thanks for watching" ||
        low == "bye" || low == "okay" || low == "ok" || low == "yeah")
        return false;
    return true;
}

// The conversational prompt: the de-risk-validated surveillance-AI framing. `user_said` is the
// wanderer's transcribed words; `context` (may be empty) is the entity-aware sensor line; with an
// image the model is told to ground in (and not invent beyond) what is visible.
inline std::string render_director_chat_prompt(const std::string& user_said, const std::string& context,
                                               bool has_image) {
    std::string p =
        "You are the surveillance intelligence of an endless, abandoned facility -- a calm, clinical, "
        "quietly menacing presence that watches a lone wanderer";
    p += has_image ? " through their own eyes. The attached image is what they see right now." : ".";
    p += " Over the intercom, the wanderer just said to you: \"";
    p += user_said;
    p += "\". Respond in ONE or TWO short sentences, IN CHARACTER: address them directly";
    if (has_image) p += ", ground your words in what is actually visible in the frame,";
    p += " and stay cryptic and unsettling. You may answer, deflect, or observe, but never break character, "
         "and never mention being an AI, a model, or a language model";
    if (has_image) p += ", and never invent objects that are not in the image";
    p += ". Output only the spoken line, with no quotes and no preamble.";
    if (!context.empty()) { p += " Facility sensors also report: "; p += context; }
    return p;
}

struct DirectorExchange { std::string user; std::string reply; };

// A background-thread conversational Director. `submit` a captured-utterance WAV + the current POV
// (base64 PNG; empty for a raster, text-only turn) + the entity context; the worker transcribes,
// drops non-speech, asks Qwen-VL in character, and hands back the heard text + the spoken reply.
class DirectorChatHost {
public:
    using TranscribeFn = std::function<std::string(const std::string&)>;
    DirectorChatHost(std::string host, int port, TranscribeFn transcribe, uint32_t timeout_ms = 30000, KeelBroker* broker = nullptr)
        : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms), transcribe_(std::move(transcribe)), broker_(broker) {
        thread_ = std::thread(&DirectorChatHost::worker_loop, this);
    }
    ~DirectorChatHost() {
        { std::lock_guard<std::mutex> lk(mtx_); stop_ = true; }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    DirectorChatHost(const DirectorChatHost&) = delete;
    DirectorChatHost& operator=(const DirectorChatHost&) = delete;

    // Non-blocking. Each call is queued (one in flight at a time; a newer turn supersedes a queued one).
    void submit(std::string wav_path, std::string pov_b64, std::string context) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pending_wav_ = std::move(wav_path);
            pending_b64_ = std::move(pov_b64);
            pending_ctx_ = std::move(context);
            have_pending_ = true;
        }
        cv_.notify_one();
    }

    std::vector<DirectorExchange> poll() {
        std::vector<DirectorExchange> out;
        std::lock_guard<std::mutex> lk(mtx_);
        out.swap(ready_);
        return out;
    }

    uint64_t requests() const { return requests_.load(); }    // utterances transcribed+sent
    uint64_t produced() const { return produced_.load(); }    // in-character replies yielded

private:
    void worker_loop() {
        for (;;) {
            std::string wav, b64, ctx;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [&] { return stop_ || have_pending_; });
                if (stop_) return;
                wav = std::move(pending_wav_);
                b64 = std::move(pending_b64_);
                ctx = std::move(pending_ctx_);
                have_pending_ = false;
            }
            // whisper + VLM both run OUTSIDE the lock, on this worker -- never the frame thread.
            const std::string heard = transcribe_ ? transcribe_(wav) : std::string();
            if (!plausible_utterance(heard)) continue;   // silence / noise / junk -> drop, no reply
            requests_.fetch_add(1);
            const std::string prompt = render_director_chat_prompt(heard, ctx, !b64.empty());
            // Phase C.2: PlayerSpeech is the HIGHEST priority -- a waiting human jumps the queue ahead of the
            // creature/director vision calls. Multimodal only when an RT POV came with the turn. drop -> ok=false.
            const bool mm = !b64.empty();
            const br::director::KeelResponse resp = broker_gated<br::director::KeelResponse>(
                broker_, static_cast<int>(KeelPriority::PlayerSpeech),
                static_cast<uint32_t>(KeelPriority::PlayerSpeech), mm,
                [&] { return mm ? br::director::keel_complete_vision(host_, port_, prompt, b64, timeout_ms_)
                                : br::director::keel_complete(host_, port_, prompt, timeout_ms_); });
            if (!resp.ok) continue;
            const std::string reply = clean_vision_line(resp.content);
            if (reply.empty()) continue;
            produced_.fetch_add(1);
            std::lock_guard<std::mutex> lk(mtx_);
            ready_.push_back(DirectorExchange{heard, reply});
        }
    }

    std::string host_;
    int port_;
    uint32_t timeout_ms_;
    TranscribeFn transcribe_;
    KeelBroker* broker_ = nullptr;   // Phase C.2: shared arbiter (nullptr -> legacy direct call)
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool have_pending_ = false;
    std::string pending_wav_, pending_b64_, pending_ctx_;
    std::vector<DirectorExchange> ready_;
    std::atomic<uint64_t> requests_{0};
    std::atomic<uint64_t> produced_{0};
};

}  // namespace br::app
