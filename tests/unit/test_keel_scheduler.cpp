// test_keel_scheduler -- Phase C: the pure LLM-request arbitration core. It is thread-free, so these
// are DETERMINISTIC property tests of the scheduling LAWS (priority order, the single multimodal slot,
// latest-wins per class, the concurrency cap, FIFO tie-break) -- no threads, no flakiness. The
// Externality Principle: assert structural invariants, not LLM output.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>

#include "keel_broker.h"
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

// --- Phase C.2: try_admit (the broker primitive) -- still pure, still deterministic --------------
TEST_CASE("try_admit admits only the highest-priority caller, then the next", "[phaseC2][keel]") {
    KeelScheduler s(4);
    const KeelTicket brain  = s.add(pri(KeelPriority::ShoggothBrain), 1, false);
    const KeelTicket vision = s.add(pri(KeelPriority::ShoggothVision), 2, true);
    const KeelTicket speech = s.add(pri(KeelPriority::PlayerSpeech), 3, false);
    REQUIRE_FALSE(s.try_admit(brain));    // lowest -> not its turn (state untouched)
    REQUIRE_FALSE(s.try_admit(vision));   // outranked by speech
    REQUIRE(s.try_admit(speech));         // the human wins
    REQUIRE_FALSE(s.try_admit(speech));   // already Running, not Pending
    s.finish(speech);
    REQUIRE_FALSE(s.try_admit(brain));    // vision (40) still outranks brain (10)
    REQUIRE(s.try_admit(vision));
    s.finish(vision);
    REQUIRE(s.try_admit(brain));          // now the lowest gets in
}

TEST_CASE("try_admit honours the single multimodal slot and rejects invalid tickets", "[phaseC2][keel]") {
    KeelScheduler s(4);
    const KeelTicket v1 = s.add(pri(KeelPriority::ShoggothVision), 1, true);
    const KeelTicket v2 = s.add(pri(KeelPriority::DirectorVision), 2, true);
    const KeelTicket tx = s.add(pri(KeelPriority::ShoggothBrain), 3, false);
    REQUIRE(s.try_admit(v1));             // first multimodal in
    REQUIRE_FALSE(s.try_admit(v2));       // second multimodal blocked by the slot
    REQUIRE(s.try_admit(tx));             // text runs alongside (cap 4)
    s.finish(v1);
    REQUIRE(s.try_admit(v2));             // slot freed
    REQUIRE_FALSE(s.try_admit(0u));       // invalid ticket
    REQUIRE_FALSE(s.try_admit(99999u));   // unknown ticket
}

// --- Phase C.2: the THREADED KeelBroker. Concurrency tests assert MUST-happen outcomes (a held slot
// CANNOT admit a second multimodal; release/shutdown MUST wake a waiter) with generous bounded waits,
// so they are robust, not timing-flaky. A real deadlock regression surfaces as a ctest timeout. -------
TEST_CASE("KeelBroker: single-threaded acquire/release cycles + post-shutdown is graceful", "[phaseC2][keel][broker]") {
    KeelBroker b(2);
    KeelTicket t = 0;
    REQUIRE(b.acquire(t, pri(KeelPriority::ShoggothBrain), 1, false) == KeelBroker::Grant::Admitted);
    REQUIRE(t != 0u);
    b.release(t);
    KeelTicket t2 = 0;
    REQUIRE(b.acquire(t2, pri(KeelPriority::ShoggothVision), 2, true) == KeelBroker::Grant::Admitted);
    b.release(t2);
    b.shutdown();
    KeelTicket t3 = 0;
    REQUIRE(b.acquire(t3, pri(KeelPriority::PlayerSpeech), 3, false) == KeelBroker::Grant::Shutdown);
}

TEST_CASE("KeelBroker: a held multimodal slot blocks a second vision across threads, freed on release", "[phaseC2][keel][broker]") {
    using namespace std::chrono_literals;
    KeelBroker b(4);  // cap high: ONLY the single multimodal slot can block here
    KeelTicket held = 0;
    REQUIRE(b.acquire(held, pri(KeelPriority::ShoggothVision), 1, true) == KeelBroker::Grant::Admitted);
    auto fut = std::async(std::launch::async, [&] {
        KeelTicket t = 0;
        return b.acquire(t, pri(KeelPriority::DirectorVision), 2, true);  // a 2nd multimodal -> must block
    });
    REQUIRE(fut.wait_for(200ms) == std::future_status::timeout);  // genuinely blocked: admitting a 2nd mm is impossible
    b.release(held);                                              // free the slot
    REQUIRE(fut.wait_for(3s) == std::future_status::ready);       // release MUST wake + admit it
    REQUIRE(fut.get() == KeelBroker::Grant::Admitted);
    b.shutdown();                                                 // backstop: no waiter left blocked
}

TEST_CASE("KeelBroker: shutdown wakes a blocked waiter", "[phaseC2][keel][broker]") {
    using namespace std::chrono_literals;
    KeelBroker b(4);
    KeelTicket held = 0;
    REQUIRE(b.acquire(held, pri(KeelPriority::ShoggothVision), 1, true) == KeelBroker::Grant::Admitted);
    auto fut = std::async(std::launch::async, [&] {
        KeelTicket t = 0;
        return b.acquire(t, pri(KeelPriority::DirectorVision), 2, true);  // blocks on the held mm slot
    });
    REQUIRE(fut.wait_for(200ms) == std::future_status::timeout);  // blocked
    b.shutdown();                                                 // must wake every blocked acquire()
    REQUIRE(fut.wait_for(3s) == std::future_status::ready);
    REQUIRE(fut.get() == KeelBroker::Grant::Shutdown);
}

TEST_CASE("KeelBroker: a text request is admitted alongside a held multimodal one", "[phaseC2][keel][broker]") {
    using namespace std::chrono_literals;
    KeelBroker b(2);
    KeelTicket mm = 0;
    REQUIRE(b.acquire(mm, pri(KeelPriority::ShoggothVision), 1, true) == KeelBroker::Grant::Admitted);
    auto fut = std::async(std::launch::async, [&] {
        KeelTicket t = 0;
        return b.acquire(t, pri(KeelPriority::ShoggothBrain), 2, false);  // text: never waits on the mm slot
    });
    REQUIRE(fut.wait_for(3s) == std::future_status::ready);  // admitted promptly (1 text + 1 mm == cap 2)
    REQUIRE(fut.get() == KeelBroker::Grant::Admitted);
    b.release(mm);
    b.shutdown();
}
