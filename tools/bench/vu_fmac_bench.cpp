// A/B benchmark for the COP2 macro FMAC lowerings (docs/architecture/benchmark-methodology.md).
//   legacy : pre-Phase-5 SSE path (host IEEE op + PS2 range fix-up; wrong contract, kept for measurement only)
//   lanes  : ps2_vu_fmac::add/sub/mul once per lane (Phase 5 commit 1d7629d..d77d5b3)
//   sse2   : ps2_vu_fmac::add4/sub4/mul4 (four lanes at once, what PS2_VADD/VSUB/VMUL use now)
// Each variant runs over the same 65,536 operand vectors; one run = one pass; reported: median, min and max of 31
// runs after 3 warm-up runs, in ns per 4-lane operation. Results of lanes and sse2 are compared bit for bit first.
// usage: ps2x_bench_vu_fmac [runs]
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
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

    template <uint32_t (*Op)(uint32_t, uint32_t)>
    __m128 lanes(__m128 a, __m128 b)
    {
        alignas(16) uint32_t la[4], lb[4], lr[4];
        _mm_store_ps(reinterpret_cast<float *>(la), a);
        _mm_store_ps(reinterpret_cast<float *>(lb), b);
        for (int i = 0; i < 4; ++i)
            lr[i] = Op(la[i], lb[i]);
        return _mm_load_ps(reinterpret_cast<const float *>(lr));
    }

    struct Stats
    {
        double median, min, max;
    };

    // The operation is a template argument so it inlines into the loop, as PS2_VADD does in recompiled code (a
    // function pointer would add an indirect call and the x64 ABI's by-pointer vector passing to every variant).
    template <__m128 (*Op)(__m128, __m128)>
    Stats measure(const std::vector<__m128> &a, const std::vector<__m128> &b, std::vector<__m128> &out, int runs)
    {
        std::vector<double> ns;
        for (int r = -3; r < runs; ++r)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i < a.size(); ++i)
                out[i] = Op(a[i], b[i]);
            const auto t1 = std::chrono::steady_clock::now();
            if (r >= 0)
                ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(a.size()));
        }
        std::sort(ns.begin(), ns.end());
        return {ns[ns.size() / 2], ns.front(), ns.back()};
    }

    void fill(std::vector<__m128> &a, std::vector<__m128> &b, bool inRange, Xorshift64 &rng)
    {
        for (size_t i = 0; i < a.size(); ++i)
        {
            alignas(16) uint32_t x[4], y[4];
            for (int l = 0; l < 4; ++l)
            {
                if (inRange)
                {
                    const uint32_t e = 60u + rng.next() % 130u;
                    x[l] = (rng.next() & 0x807FFFFFu) | (e << 23);
                    y[l] = (rng.next() & 0x807FFFFFu) | ((e - rng.next() % 30u) << 23);
                }
                else
                {
                    x[l] = rng.next();
                    y[l] = rng.next();
                }
            }
            a[i] = _mm_load_ps(reinterpret_cast<const float *>(x));
            b[i] = _mm_load_ps(reinterpret_cast<const float *>(y));
        }
    }

    bool same(const std::vector<__m128> &x, const std::vector<__m128> &y)
    {
        return std::memcmp(x.data(), y.data(), x.size() * sizeof(__m128)) == 0;
    }

    template <__m128 (*Legacy)(__m128, __m128), __m128 (*Lane)(__m128, __m128), __m128 (*Sse2)(__m128, __m128)>
    int row(const char *name, const std::vector<__m128> &a, const std::vector<__m128> &b, std::vector<__m128> &out,
            std::vector<__m128> &check, int runs)
    {
        measure<Lane>(a, b, check, 1);
        measure<Sse2>(a, b, out, 1);
        if (!same(check, out))
        {
            std::printf("  %s: lanes and sse2 DIFFER - benchmark invalid\n", name);
            return 1;
        }
        const Stats l = measure<Legacy>(a, b, out, runs);
        const Stats p = measure<Lane>(a, b, out, runs);
        const Stats s = measure<Sse2>(a, b, out, runs);
        std::printf("  %s  legacy %6.2f [%.2f..%.2f]  lanes %6.2f [%.2f..%.2f]  sse2 %6.2f [%.2f..%.2f]  lanes/sse2 %.2fx\n",
                    name, l.median, l.min, l.max, p.median, p.min, p.max, s.median, s.min, s.max, p.median / s.median);
        return 0;
    }
}

int main(int argc, char **argv)
{
    const int runs = argc > 1 ? std::max(1, std::atoi(argv[1])) : 31;
    const size_t n = 1u << 16;
    std::vector<__m128> a(n), b(n), out(n), check(n);
    Xorshift64 rng{0x9E3779B97F4A7C15ull};
    int failures = 0;
    std::printf("ns per 4-lane op, median [min..max] of %d runs over %zu vectors\n", runs, n);
    for (const bool inRange : {true, false})
    {
        fill(a, b, inRange, rng);
        std::printf("%s operands\n", inRange ? "normal (exponents 31..189, gaps 0..29)" : "random bits (all classes)");
        failures += row<ps2_vadd_legacy, lanes<ps2_vu_fmac::add>, ps2_vadd>("add", a, b, out, check, runs);
        failures += row<ps2_vsub_legacy, lanes<ps2_vu_fmac::sub>, ps2_vsub>("sub", a, b, out, check, runs);
        failures += row<ps2_vmul_legacy, lanes<ps2_vu_fmac::mul>, ps2_vmul>("mul", a, b, out, check, runs);
    }
    return failures;
}
