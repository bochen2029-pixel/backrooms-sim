#pragma once
//
// app/keel_scheduler.h — Phase C (SHOGGOTH_PLAN.md §5/§7): the PURE arbitration core for the single
// llama-server backend. It decides WHICH pending LLM request runs next so the (up to five) consumers
// — player-speech, shoggoth-vision, director-vision, director-narration, shoggoth-brain — don't
// starve each other or overlap expensive multimodal (vision) calls.
//
// THREAD-FREE BY DESIGN. This is the decision logic only; the threaded `KeelBroker` (Phase C.2) wraps
// it with a mutex + condition_variable and runs each admitted request's thunk on the caller's host
// thread. Keeping the logic pure means its LAWS — priority order, the single-multimodal slot,
// latest-wins-per-class, the concurrency cap — are unit-testable WITHOUT threads (zero flakiness).
// That is the Externality Principle in practice: the tests assert structural invariants, not LLM
// output. Integration into the live hosts (DirectorVisionHost / DirectorChatHost / ShoggothBrainHost
// + the future ShoggothVisionHost/HearingHost) is Phase C.2 and needs KEEL :7071 up to soak.
//
#include <cstdint>
#include <vector>

namespace br::app {

// Higher value = more urgent. The five live consumers (the human waiting outranks everything; the
// creature's own eyes outrank the director's; cheap text brains are lowest because they are frequent).
enum class KeelPriority : int {
    ShoggothBrain     = 10,   // cheap text, every ~3 s — lowest
    DirectorNarration = 20,   // cheap text, ~18 s
    DirectorVision    = 30,   // expensive multimodal, ~28 s
    ShoggothVision    = 40,   // expensive multimodal — the creature's own POV
    PlayerSpeech      = 50,   // a human is blocked on the reply — highest
};

using KeelTicket = uint64_t;   // 0 == invalid

// Pure scheduler. Single-threaded use (the broker holds a lock around it). All decisions are a
// deterministic function of the registered requests + their states.
class KeelScheduler {
public:
    explicit KeelScheduler(int max_concurrency = 2)
        : max_concurrency_(max_concurrency < 1 ? 1 : max_concurrency) {}

    // Register a pending request. LATEST-WINS PER CLASS: any still-PENDING request sharing this
    // class_id is marked Superseded (its host should drop it — matches the existing "latest-wins
    // submit" hosts). Returns a unique, monotonic ticket.
    KeelTicket add(int priority, uint32_t class_id, bool multimodal) {
        for (auto& r : reqs_)
            if (r.state == Pending && r.class_id == class_id) r.state = Superseded;
        const KeelTicket t = ++last_ticket_;
        reqs_.push_back(Req{t, priority, class_id, multimodal, Pending});
        return t;
    }

    // Did a newer same-class add() supersede this ticket while it was still waiting?
    bool is_superseded(KeelTicket t) const {
        for (const auto& r : reqs_)
            if (r.ticket == t) return r.state == Superseded;
        return false;  // unknown / already finished
    }

    // The highest-priority PENDING request admissible RIGHT NOW — i.e. below the concurrency cap and,
    // if it is multimodal, only when no multimodal call is already running (the single GPU/VRAM slot).
    // Ties break FIFO (older ticket first). Marks the winner Running and returns its ticket; 0 if
    // nothing is admissible at this moment.
    KeelTicket admit_next() {
        if (running_count() >= max_concurrency_) return 0;
        const bool mm_busy = multimodal_running();
        KeelTicket best = 0; int best_prio = 0; size_t best_i = 0;
        for (size_t i = 0; i < reqs_.size(); ++i) {
            const Req& r = reqs_[i];
            if (r.state != Pending) continue;
            if (r.multimodal && mm_busy) continue;                 // single multimodal slot
            const bool better = (best == 0) || (r.priority > best_prio) ||
                                (r.priority == best_prio && r.ticket < best);  // FIFO tie-break
            if (better) { best = r.ticket; best_prio = r.priority; best_i = i; }
        }
        if (best != 0) reqs_[best_i].state = Running;
        return best;
    }

    // Broker primitive (Phase C.2): admit `mine` IFF it is the highest-priority admissible request right
    // now (below the concurrency cap, and — if multimodal — only when no multimodal call is running). On
    // success marks `mine` Running and returns true; otherwise leaves all state untouched and returns false.
    // Same selection + FIFO tie-break as admit_next, but it admits ONLY the caller's own ticket — so each
    // threaded KeelBroker waiter admits only itself and the acquire loop is trivially correct (no
    // cross-thread admission). mine==0 / unknown / not-Pending / mm-blocked -> false.
    bool try_admit(KeelTicket mine) {
        if (mine == 0 || running_count() >= max_concurrency_) return false;
        const bool mm_busy = multimodal_running();
        KeelTicket best = 0; int best_prio = 0; size_t mine_i = reqs_.size();
        for (size_t i = 0; i < reqs_.size(); ++i) {
            const Req& r = reqs_[i];
            if (r.ticket == mine && r.state == Pending) mine_i = i;
            if (r.state != Pending) continue;
            if (r.multimodal && mm_busy) continue;                 // single multimodal slot
            const bool better = (best == 0) || (r.priority > best_prio) ||
                                (r.priority == best_prio && r.ticket < best);  // FIFO tie-break
            if (better) { best = r.ticket; best_prio = r.priority; }
        }
        if (best == mine && mine_i < reqs_.size()) { reqs_[mine_i].state = Running; return true; }
        return false;
    }

    // A Running request completed — remove it (frees its concurrency + multimodal slot).
    void finish(KeelTicket t) {
        for (size_t i = 0; i < reqs_.size(); ++i)
            if (reqs_[i].ticket == t) { reqs_.erase(reqs_.begin() + static_cast<std::ptrdiff_t>(i)); return; }
    }
    // Drop a superseded/cancelled request that never ran (alias of finish — same bookkeeping).
    void drop(KeelTicket t) { finish(t); }

    size_t pending() const { size_t n = 0; for (const auto& r : reqs_) if (r.state == Pending) ++n; return n; }
    int    running_count() const { int n = 0; for (const auto& r : reqs_) if (r.state == Running) ++n; return n; }
    bool   multimodal_running() const {
        for (const auto& r : reqs_) if (r.state == Running && r.multimodal) return true;
        return false;
    }

private:
    enum State { Pending, Running, Superseded };
    struct Req { KeelTicket ticket; int priority; uint32_t class_id; bool multimodal; State state; };
    std::vector<Req> reqs_;
    KeelTicket last_ticket_ = 0;
    int max_concurrency_;
};

}  // namespace br::app
