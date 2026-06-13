#pragma once
//
// core/rng.h — the sole random-number generator of the deterministic sim core.
//
// Algorithm: PCG64 (XSL-RR 128/64), per M.E. O'Neill, "PCG: A Family of Simple
// Fast Space-Efficient Statistically Good Algorithms for Random Number
// Generation" (2014). 128-bit LCG state, 64-bit output. The canonical pcg64
// multiplier and a fixed default stream are used.
//
// Invariants this type exists to uphold:
//   INV-1 Determinism — a seeded Pcg64 produces a bit-identical sequence on
//   every platform/run. No wall-clock, no global state, no floating point in
//   the integer path. The full 128-bit state is exposed for WorldState hashing.
//
// Portability: implemented with explicit 64x64 -> 128 multiplies (no reliance
// on a 128-bit compiler type such as __int128, which MSVC lacks).
//
#include <cstdint>

namespace br::core {

class Pcg64 {
public:
    // Seed from a 64-bit world seed. The stream selector defaults to a fixed
    // constant so a bare WorldSeed fully determines the sequence.
    explicit Pcg64(uint64_t seed, uint64_t stream = kDefaultStream) noexcept {
        seed_full(0u, seed, 0u, stream);
    }

    // Full 128-bit seeding: initstate = (state_hi:state_lo), initseq =
    // (seq_hi:seq_lo). Used by tests and any future multi-stream needs.
    Pcg64(uint64_t state_hi, uint64_t state_lo,
          uint64_t seq_hi, uint64_t seq_lo) noexcept {
        seed_full(state_hi, state_lo, seq_hi, seq_lo);
    }

    // Next 64-bit output. Output is derived from the pre-step state (standard
    // PCG ordering), then the LCG state is advanced.
    uint64_t next_u64() noexcept {
        const uint64_t hi = state_hi_;
        const uint64_t lo = state_lo_;
        const uint64_t xored = hi ^ lo;                 // XSL: xor the halves
        const unsigned rot = static_cast<unsigned>(hi >> 58);  // top 6 bits
        const uint64_t out = rotr64(xored, rot);        // RR: rotate right
        step();
        return out;
    }

    // Uniform double in [0, 1) using the top 53 mantissa bits.
    double next_double() noexcept {
        return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
    }

    // Uniform float in [0, 1) using the top 24 bits.
    float next_float() noexcept {
        return static_cast<float>(next_u64() >> 40) * (1.0f / 16777216.0f);
    }

    // Unbiased integer in [0, bound) via Lemire's multiply-shift rejection.
    // bound == 0 returns 0 (degenerate).
    uint64_t bounded(uint64_t bound) noexcept {
        if (bound == 0u) return 0u;
        uint64_t hi, lo;
        umul(next_u64(), bound, hi, lo);
        if (lo < bound) {
            const uint64_t threshold = (0u - bound) % bound;  // 2^64 mod bound
            while (lo < threshold) {
                umul(next_u64(), bound, hi, lo);
            }
        }
        return hi;
    }

    // Raw 128-bit state — hashable for per-tick WorldState hashing (INV-1).
    uint64_t state_hi() const noexcept { return state_hi_; }
    uint64_t state_lo() const noexcept { return state_lo_; }

private:
    // Canonical pcg64 LCG multiplier (128-bit, hi:lo).
    static constexpr uint64_t kMulHi = 0x2360ed051fc65da4ULL;
    static constexpr uint64_t kMulLo = 0x4385df649fccf645ULL;
    // Fixed default stream selector (arbitrary odd-ish constant; becomes odd
    // increment after the (<<1)|1 transform in seeding).
    static constexpr uint64_t kDefaultStream = 0xda3e39cb94b95bdbULL;

    uint64_t state_hi_ = 0, state_lo_ = 0;  // LCG state (128-bit)
    uint64_t inc_hi_ = 0,   inc_lo_ = 0;    // LCG increment (128-bit, odd)

    static uint64_t rotr64(uint64_t v, unsigned r) noexcept {
        r &= 63u;
        return r ? ((v >> r) | (v << (64u - r))) : v;
    }

    // 64x64 -> 128 unsigned multiply (schoolbook on 32-bit limbs; portable).
    static void umul(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) noexcept {
        const uint64_t a0 = a & 0xffffffffULL, a1 = a >> 32;
        const uint64_t b0 = b & 0xffffffffULL, b1 = b >> 32;
        const uint64_t p00 = a0 * b0;
        const uint64_t p01 = a0 * b1;
        const uint64_t p10 = a1 * b0;
        const uint64_t p11 = a1 * b1;
        const uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffULL) + (p10 & 0xffffffffULL);
        lo = (p00 & 0xffffffffULL) | (mid << 32);
        hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    }

    static void add128(uint64_t ahi, uint64_t alo, uint64_t bhi, uint64_t blo,
                       uint64_t& rhi, uint64_t& rlo) noexcept {
        const uint64_t sum = alo + blo;
        rlo = sum;
        rhi = ahi + bhi + (sum < alo ? 1u : 0u);  // carry
    }

    // 128x128 -> low 128 bits (modular). Cross terms only reach the high half.
    static void mul128(uint64_t ahi, uint64_t alo, uint64_t bhi, uint64_t blo,
                       uint64_t& rhi, uint64_t& rlo) noexcept {
        uint64_t hi, lo;
        umul(alo, blo, hi, lo);
        hi += alo * bhi;
        hi += ahi * blo;
        rhi = hi;
        rlo = lo;
    }

    void step() noexcept {
        uint64_t phi, plo;
        mul128(state_hi_, state_lo_, kMulHi, kMulLo, phi, plo);
        add128(phi, plo, inc_hi_, inc_lo_, state_hi_, state_lo_);
    }

    void seed_full(uint64_t shi, uint64_t slo, uint64_t qhi, uint64_t qlo) noexcept {
        // increment = (initseq << 1) | 1  (guarantees an odd 128-bit increment)
        inc_hi_ = (qhi << 1) | (qlo >> 63);
        inc_lo_ = (qlo << 1) | 1u;
        state_hi_ = 0;
        state_lo_ = 0;
        step();
        add128(state_hi_, state_lo_, shi, slo, state_hi_, state_lo_);
        step();
    }
};

}  // namespace br::core
