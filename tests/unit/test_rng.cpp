// test_rng — the seed unit test. Exercises the canonical core RNG (Pcg64),
// which is the determinism oracle for the whole build (INV-1).
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdio>

#include "core/rng.h"

using br::core::Pcg64;

TEST_CASE("Pcg64 is deterministic for a given seed", "[rng][determinism]") {
    Pcg64 a(12345u);
    Pcg64 b(12345u);
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(a.next_u64() == b.next_u64());
    }
}

TEST_CASE("Pcg64 sequences differ across seeds", "[rng]") {
    Pcg64 a(1u);
    Pcg64 b(2u);
    int diffs = 0;
    for (int i = 0; i < 64; ++i) {
        if (a.next_u64() != b.next_u64()) ++diffs;
    }
    REQUIRE(diffs > 60);  // overwhelmingly different streams
}

TEST_CASE("Pcg64 next_double mean is ~0.5", "[rng][stats]") {
    Pcg64 r(99u);
    constexpr int kN = 200000;
    double sum = 0.0;
    for (int i = 0; i < kN; ++i) sum += r.next_double();
    const double mean = sum / static_cast<double>(kN);
    REQUIRE(mean > 0.49);
    REQUIRE(mean < 0.51);
}

TEST_CASE("Pcg64 next_double stays in [0,1)", "[rng][stats]") {
    Pcg64 r(0xabcdu);
    for (int i = 0; i < 100000; ++i) {
        const double d = r.next_double();
        REQUIRE(d >= 0.0);
        REQUIRE(d < 1.0);
    }
}

TEST_CASE("Pcg64 bounded is in range and roughly uniform", "[rng][stats]") {
    Pcg64 r(7u);
    std::array<int, 6> hist{};
    constexpr int kN = 60000;
    for (int i = 0; i < kN; ++i) {
        const uint64_t v = r.bounded(6u);
        REQUIRE(v < 6u);
        hist[static_cast<size_t>(v)] += 1;
    }
    for (int count : hist) {
        REQUIRE(count > 9000);  // each of 6 faces ~10000
    }
}

TEST_CASE("Pcg64 bounded(0) is the degenerate 0", "[rng]") {
    Pcg64 r(3u);
    REQUIRE(r.bounded(0u) == 0u);
}

TEST_CASE("Pcg64 exposes advancing 128-bit state", "[rng]") {
    Pcg64 r(5u);
    const uint64_t h0 = r.state_hi();
    const uint64_t l0 = r.state_lo();
    (void)r.next_u64();
    REQUIRE((r.state_hi() != h0 || r.state_lo() != l0));
}

// Locked output stream — INV-1 regression guard. Filled from the hidden
// "[.capture]" case after the first build (see SESSION_LOG). Any later change
// to the RNG algorithm or constants breaks these vectors.
TEST_CASE("Pcg64 locked stream regression", "[rng][golden]") {
    // Captured from this implementation (the canonical pcg64 XSL-RR 128/64).
    // These vectors are the determinism oracle: any drift in algorithm or
    // constants breaks them. Updating them requires a deliberate change + note.
    Pcg64 r0(0u);
    const std::array<uint64_t, 4> kSeed0 = {
        0x203b852c55d1f963ULL, 0x241921ff0a5798e0ULL,
        0xc843e134b390269aULL, 0x55e753a68f9061bdULL,
    };
    for (uint64_t expect : kSeed0) {
        REQUIRE(r0.next_u64() == expect);
    }

    Pcg64 r1(12345u);
    const std::array<uint64_t, 4> kSeed12345 = {
        0xfaacbeffa9791a63ULL, 0x7ad44031602e9ed2ULL,
        0x2c7241669983995eULL, 0xe5f2ad44d7eb3af2ULL,
    };
    for (uint64_t expect : kSeed12345) {
        REQUIRE(r1.next_u64() == expect);
    }
}

// Hidden helper (tag begins with '.') used once to capture the locked vectors.
// Run explicitly with:  unit_tests "[capture]"
TEST_CASE("capture pcg64 vectors", "[.capture]") {
    for (uint64_t seed : {uint64_t{0}, uint64_t{12345}}) {
        Pcg64 r(seed);
        std::printf("seed %llu:", static_cast<unsigned long long>(seed));
        for (int i = 0; i < 4; ++i) {
            std::printf(" 0x%016llxULL,", static_cast<unsigned long long>(r.next_u64()));
        }
        std::printf("\n");
    }
    SUCCEED();
}
