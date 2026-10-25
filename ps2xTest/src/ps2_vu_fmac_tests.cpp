// Differential, property and hardware-anchor tests for the VU FMAC contract (ps2xRuntime/include/ps2_vu_fmac.h).
// The integer reference (*Ref) and the double-based lowering (add/sub/mul) share no code; every comparison below
// runs both on the same operands and reports the first difference, minimised.
#include "MiniTest.h"
#include "ps2_runtime_macros.h"
#include "ps2_vu_fmac.h"

#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using Op = uint32_t (*)(uint32_t, uint32_t);

    struct Xorshift64
    {
        uint64_t s;
        uint32_t next()
        {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            return static_cast<uint32_t>(s >> 16);
        }
    };

    std::string hex(uint32_t v)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "%08x", v);
        return text;
    }

    // Boundary operands: exponents around 0, 1, the gap threshold, the middle, and 254/255; mantissas 0, 1, 2, the
    // half point and all-ones; both signs.
    std::vector<uint32_t> boundaryOperands()
    {
        const uint32_t exponents[] = {0u, 1u, 2u, 24u, 25u, 26u, 100u, 126u, 127u, 128u, 151u, 152u, 153u, 230u, 253u, 254u, 255u};
        const uint32_t mantissas[] = {0u, 1u, 2u, 0x3FFFFFu, 0x400000u, 0x400001u, 0x7FFFFEu, 0x7FFFFFu};
        std::vector<uint32_t> out;
        for (uint32_t e : exponents)
            for (uint32_t m : mantissas)
                for (uint32_t s : {0u, 0x80000000u})
                    out.push_back(s | (e << 23) | m);
        return out;
    }

    // Shrinks a failing pair: clear mantissa bits of a and b one at a time while the two implementations still differ.
    void minimise(Op fast, Op ref, uint32_t &a, uint32_t &b)
    {
        for (bool changed = true; changed;)
        {
            changed = false;
            for (uint32_t *x : {&a, &b})
                for (int bit = 22; bit >= 0; --bit)
                {
                    const uint32_t trial = *x & ~(1u << bit);
                    if (trial == *x)
                        continue;
                    const uint32_t keep = *x;
                    *x = trial;
                    if (fast(a, b) != ref(a, b))
                        changed = true;
                    else
                        *x = keep;
                }
        }
    }

    // Runs fast and ref on every pair; returns "" or the first (minimised) difference.
    std::string compare(Op fast, Op ref, const std::vector<std::pair<uint32_t, uint32_t>> &pairs, size_t &checked)
    {
        for (const auto &[a0, b0] : pairs)
        {
            ++checked;
            if (fast(a0, b0) == ref(a0, b0))
                continue;
            uint32_t a = a0, b = b0;
            minimise(fast, ref, a, b);
            return "first difference " + hex(a0) + ", " + hex(b0) + " (minimised " + hex(a) + ", " + hex(b) +
                   "): lowering " + hex(fast(a, b)) + ", reference " + hex(ref(a, b));
        }
        return {};
    }

    std::vector<std::pair<uint32_t, uint32_t>> corpus()
    {
        std::vector<std::pair<uint32_t, uint32_t>> pairs;
        const std::vector<uint32_t> edge = boundaryOperands();
        for (uint32_t a : edge)
            for (uint32_t b : edge)
                pairs.emplace_back(a, b);
        // Structured: every exponent gap 0..30, both sign combinations, random mantissas.
        Xorshift64 rng{0x9E3779B97F4A7C15ull};
        for (uint32_t gap = 0; gap <= 30; ++gap)
            for (int i = 0; i < 2000; ++i)
            {
                const uint32_t ea = 30u + rng.next() % 190u;
                const uint32_t a = (rng.next() & 0x80000000u) | (ea << 23) | (rng.next() & 0x7FFFFFu);
                const uint32_t b = (rng.next() & 0x80000000u) | ((ea - gap) << 23) | (rng.next() & 0x7FFFFFu);
                pairs.emplace_back(a, b);
                pairs.emplace_back(b, a);
            }
        // Unstructured: random bit patterns (every exponent class, including 0 and 255).
        for (int i = 0; i < 400000; ++i)
            pairs.emplace_back(rng.next(), rng.next());
        return pairs;
    }

    uint32_t laneBits(__m128 v, int lane)
    {
        alignas(16) uint32_t lanes[4];
        _mm_store_ps(reinterpret_cast<float *>(lanes), v);
        return lanes[lane];
    }
}

void register_ps2_vu_fmac_tests()
{
    MiniTest::Case("PS2VuFmac", [](TestCase &tc)
    {
        tc.Run("FMAC lowering equals the integer reference on boundary, structured and random operands", [](TestCase &t)
        {
            const auto pairs = corpus();
            for (const auto &[name, fast, ref] : {std::tuple<const char *, Op, Op>{"add", ps2_vu_fmac::add, ps2_vu_fmac::addRef},
                                                  {"sub", ps2_vu_fmac::sub, ps2_vu_fmac::subRef},
                                                  {"mul", ps2_vu_fmac::mul, ps2_vu_fmac::mulRef}})
            {
                size_t checked = 0;
                const std::string diff = compare(fast, ref, pairs, checked);
                t.IsTrue(diff.empty(), std::string(name) + ": " + diff);
                t.IsTrue(checked == pairs.size(), std::string(name) + ": every pair checked");
            }
        });

        tc.Run("FMAC contract anchors from real-PS2 results", [](TestCase &t)
        {
            // vu/games/triace: ADDi of 0xB3E2A618 (-1.05e-7) and 0x42546666 reads 0x42546666 (A1, gap >= 25).
            t.Equals(ps2_vu_fmac::addRef(0x42546666u, 0xB3E2A618u), 0x42546666u, "triace: a shifted-out addend is dropped");
            t.Equals(ps2_vu_fmac::add(0xB3E2A618u, 0x42546666u), 0x42546666u, "whatever the operand order");
            // vu/lower/efu: VU results reach 0x7FFFFFFF (exponent 255 is a value): F1/F2.
            t.Equals(ps2_vu_fmac::mulRef(0x7F800000u, 0x3F800000u), 0x7F800000u, "2^128 * 1 stays 2^128");
            t.Equals(ps2_vu_fmac::addRef(0x7FFFFFFFu, 0x7FFFFFFFu), 0x7FFFFFFFu, "max + max saturates to max");
            t.Equals(ps2_vu_fmac::mulRef(0x7FFFFFFFu, 0x7FFFFFFFu), 0x7FFFFFFFu, "max * max saturates to max");
            // Truncation (A2/M1): 1 + 2^-24 is not representable and truncates to 1; 1/3 * 3 truncates below 1.
            t.Equals(ps2_vu_fmac::addRef(0x3F800000u, 0x33800000u), 0x3F800000u, "1 + 2^-24 truncates to 1");
            t.Equals(ps2_vu_fmac::mulRef(0x3EAAAAABu, 0x40400000u), 0x3F800000u, "0x3EAAAAAB * 3 = 1.00000003 truncates to 1");
            t.Equals(ps2_vu_fmac::mulRef(0x3EAAAAAAu, 0x40400000u), 0x3F7FFFFFu, "0x3EAAAAAA * 3 truncates to 0x3F7FFFFF");
        });

        tc.Run("FMAC contract properties hold for the lowering", [](TestCase &t)
        {
            // Properties of this contract (A1/A2/M1 are symmetric), not of PS2 arithmetic in general: EE COP1 MUL.S is
            // not commutative, so no such property is asserted for it.
            Xorshift64 rng{0x2545F4914F6CDD1Dull};
            uint32_t failures = 0;
            std::string first;
            for (int i = 0; i < 200000; ++i)
            {
                const uint32_t a = rng.next(), b = rng.next();
                const uint32_t s = ps2_vu_fmac::add(a, b);
                const uint32_t p = ps2_vu_fmac::mul(a, b);
                const bool zeroSum = (s & 0x7FFFFFFFu) == 0u;
                const bool ok = s == ps2_vu_fmac::add(b, a) &&                                       // ADD commutes
                                (zeroSum || ps2_vu_fmac::add(a ^ 0x80000000u, b ^ 0x80000000u) == (s ^ 0x80000000u)) &&
                                p == ps2_vu_fmac::mul(b, a) &&                                       // MUL commutes
                                ps2_vu_fmac::mul(a ^ 0x80000000u, b) == (p ^ 0x80000000u) &&       // sign symmetry
                                ps2_vu_fmac::sub(a, a) == 0u;                                        // x - x = +0
                if (!ok && failures++ == 0)
                    first = hex(a) + ", " + hex(b);
            }
            t.Equals(failures, 0u, "property failures (first at " + first + ")");
        });

        tc.Run("COP2 macro VADD/VSUB/VMUL use the FMAC contract in every lane", [](TestCase &t)
        {
            // The generated-code entry points (PS2_VADD/PS2_VSUB/PS2_VMUL) against the scalar reference, lane by lane.
            Xorshift64 rng{0xD1B54A32D192ED03ull};
            uint32_t mismatches = 0;
            for (int i = 0; i < 50000; ++i)
            {
                alignas(16) uint32_t a[4], b[4];
                for (int l = 0; l < 4; ++l)
                {
                    a[l] = rng.next();
                    b[l] = rng.next();
                }
                const __m128 va = _mm_load_ps(reinterpret_cast<const float *>(a));
                const __m128 vb = _mm_load_ps(reinterpret_cast<const float *>(b));
                const __m128 s = PS2_VADD(va, vb), d = PS2_VSUB(va, vb), p = PS2_VMUL(va, vb);
                for (int l = 0; l < 4; ++l)
                    mismatches += (laneBits(s, l) != ps2_vu_fmac::addRef(a[l], b[l])) +
                                  (laneBits(d, l) != ps2_vu_fmac::subRef(a[l], b[l])) +
                                  (laneBits(p, l) != ps2_vu_fmac::mulRef(a[l], b[l]));
            }
            t.Equals(mismatches, 0u, "lanes differing from the reference");
        });
    });
}

