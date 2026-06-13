// gate_canary — a DELIBERATELY failing test. Always compiled, but registered
// DISABLED in CTest so the normal suite stays green. scripts/gate.ps1 runs this
// binary directly and asserts a nonzero exit, proving that a failing test is
// detected and would block a commit via the pre-commit hook. This is the
// "deliberately failing test blocks commit" exit criterion of M0.
#include <catch2/catch_test_macros.hpp>

TEST_CASE("gate canary deliberately fails", "[canary]") {
    FAIL("intentional failure: the verification harness must surface this as nonzero");
}
