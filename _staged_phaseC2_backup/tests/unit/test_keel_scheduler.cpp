// test_keel_scheduler -- Phase C: the pure LLM-request arbitration core. It is thread-free, so these
// are DETERMINISTIC property tests of the scheduling LAWS (priority order, the single multimodal slot,
// latest-wins per class, the concurrency cap, FIFO tie-break) -- no threads, no flakiness. The
// Externality Principle: assert structural invariants, not LLM output.
#include <catch2/catch_test_macros.hpp>

#include "keel_scheduler.h"

using namespace br::app;
namespace {
int pri(KeelPriority p) { return static_cast<int>(p); }
}  // namespace

TEST_CASE("the scheduler admits the highest priority first", "[phaseC][keel]") {
    KeelScheduler s(4);  // cap high enough that priority -- not the cap -- decides
    const KeelTicket brain  = s.add(pri(KeelPriority::ShoggothBrain), 1, false);
    const KeelTicket vision = s.add(pri(KeelPriority::ShoggothVision), 2, true);
    const KeelTicket speech = s.add(pri(KeelPriority::PlayerSpeech), 3, false);
    REQUIRE(s.admit_next() == speech);   // 50 -- the human is waiting
    s.finish(speech);
    REQUIRE(s.admit_next() == vision);   // 40
    s.finish(vision);
    REQUIRE(s.admit_next() == brain);    // 10
    s.finish(brain);
    REQUIRE(s.admit_next() == 0u);       // nothing left
}

TEST_CASE("the scheduler enforces a single multimodal slot", "[phaseC][keel]") {
    KeelScheduler s(4);  // cap high so ONLY the multimodal slot can block here
    const KeelTicket v1 = s.add(pri(KeelPriority::ShoggothVision), 1, true);
    const KeelTicket v2 = s.add(pri(KeelPriority::DirectorVision), 2, true);
    REQUIRE(s.admit_next() == v1);
    REQUIRE(s.multimodal_running());
    REQUIRE(s.admit_next() == 0u);       // v2 blocked by the slot (not the cap)
    s.finish(v1);
    REQUIRE(s.admit_next() == v2);       // slot freed
}

TEST_CASE("a text request runs while a multimodal call holds the slot", "[phaseC][keel]") {
    KeelScheduler s(4);
    const KeelTicket vision = s.add(pri(KeelPriority::ShoggothVision), 1, true);
    const KeelTicket text   = s.add(pri(KeelPriority::ShoggothBrain), 2, false);  // lower, but text
    REQUIRE(s.admit_next() == vision);
    REQUIRE(s.admit_next() == text);     // text never waits on the multimodal slot
    REQUIRE(s.running_count() == 2);
}

TEST_CASE("latest-wins supersedes an older pending request of the same class", "[phaseC][keel]") {
    KeelScheduler s(4);
    const KeelTicket a = s.add(pri(KeelPriority::DirectorVision), 7, true);  // class 7
    const KeelTicket b = s.add(pri(KeelPriority::DirectorVision), 7, true);  // class 7 again
    REQUIRE(s.is_superseded(a));
    REQUIRE_FALSE(s.is_superseded(b));
    REQUIRE(s.admit_next() == b);        // the superseded one is never admitted
    REQUIRE(s.admit_next() == 0u);
}

TEST_CASE("the scheduler respects the concurrency cap", "[phaseC][keel]") {
    KeelScheduler s(2);
    const KeelTicket a = s.add(pri(KeelPriority::ShoggothBrain), 1, false);
    const KeelTicket b = s.add(pri(KeelPriority::ShoggothBrain), 2, false);
    const KeelTicket c = s.add(pri(KeelPriority::ShoggothBrain), 3, false);
    REQUIRE(s.admit_next() == a);
    REQUIRE(s.admit_next() == b);
    REQUIRE(s.admit_next() == 0u);       // cap of 2 reached
    s.finish(a);
    REQUIRE(s.admit_next() == c);        // a slot freed
}

TEST_CASE("equal-priority requests admit in FIFO order", "[phaseC][keel]") {
    KeelScheduler s(1);
    const KeelTicket first  = s.add(pri(KeelPriority::DirectorNarration), 1, false);
    const KeelTicket second = s.add(pri(KeelPriority::DirectorNarration), 2, false);  // same prio, later
    REQUIRE(s.admit_next() == first);    // older ticket first
    s.finish(first);
    REQUIRE(s.admit_next() == second);
}
