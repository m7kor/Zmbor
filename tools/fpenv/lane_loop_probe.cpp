// Compiler probe for the PR #3 lane-w observation (docs/architecture/vu-ee-arithmetic-semantics.md, section 4).
// The VU micro interpreter once computed FMAC lanes with a per-lane loop over the scalar ps2_vu_fmac kernel:
//     for (int c = 0; c < 4; c++) result[c] = ps2_vu_fmac::addf(rvs[c], rvt[c]);
// The MSVC 14.51 CI build wrote lane w's result into every lane; MSVC 14.44 and GCC/Clang did not. This program
// reproduces that loop shape outside the interpreter (register file in a struct, a non-inlined switch, a local result
// array) and reports whether the lanes stay independent. It is informational: production no longer uses the loop.
// Result on CI (6af2f16): lanes independent on MSVC 14.51, GCC 13.3 and Clang 18.1, so this shape alone does not
// reproduce the observation.
// Exit code 0 = lanes independent, 1 = lanes differ from the per-lane reference (the pattern reproduced).
#include "ps2_vu_fmac.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#if defined(_MSC_VER)
#define PROBE_NOINLINE __declspec(noinline)
#else
#define PROBE_NOINLINE __attribute__((noinline))
#endif

namespace
{
    struct State
    {
        float vf[32][4];
        float acc[4];
        float q, i;
    };

    PROBE_NOINLINE void execUpper(State &s, unsigned op, unsigned fs, unsigned ft, unsigned fd)
    {
        const float *rvs = s.vf[fs];
        const float *rvt = s.vf[ft];
        const float *racc = s.acc;
        const float rq = s.q;
        const float ri = s.i;
        float result[4];
        switch (op)
        {
        case 0x28: // ADD
            for (int c = 0; c < 4; c++)
                result[c] = ps2_vu_fmac::addf(rvs[c], rvt[c]);
            break;
        case 0x2C: // SUB
            for (int c = 0; c < 4; c++)
                result[c] = ps2_vu_fmac::subf(rvs[c], rvt[c]);
            break;
        case 0x2A: // MUL
            for (int c = 0; c < 4; c++)
                result[c] = ps2_vu_fmac::mulf(rvs[c], rvt[c]);
            break;
        case 0x29: // MADD
            for (int c = 0; c < 4; c++)
                result[c] = ps2_vu_fmac::addf(racc[c], ps2_vu_fmac::mulf(rvs[c], rvt[c]));
            break;
        case 0x20: // ADDq
            for (int c = 0; c < 4; c++)
                result[c] = ps2_vu_fmac::addf(rvs[c], rq);
            break;
        case 0x22: // ADDi
            for (int c = 0; c < 4; c++)
                result[c] = ps2_vu_fmac::addf(rvs[c], ri);
            break;
        default:
            return;
        }
        std::memcpy(s.vf[fd], result, sizeof(result));
    }
}

int main()
{
    State s{};
    for (int l = 0; l < 4; ++l)
    {
        s.vf[1][l] = static_cast<float>(l + 1);
        s.vf[2][l] = static_cast<float>(10 * (l + 1));
        s.acc[l] = static_cast<float>(100 * (l + 1));
    }
    s.q = 0.5f;
    s.i = 3.0f;
    int differ = 0;
    for (const unsigned op : {0x28u, 0x2Cu, 0x2Au, 0x29u, 0x20u, 0x22u})
    {
        State t = s;
        execUpper(t, op, 1u, 2u, 3u);
        for (int l = 0; l < 4; ++l)
        {
            const float a = s.vf[1][l], b = s.vf[2][l];
            const float want = op == 0x28u ? ps2_vu_fmac::addf(a, b)
                             : op == 0x2Cu ? ps2_vu_fmac::subf(a, b)
                             : op == 0x2Au ? ps2_vu_fmac::mulf(a, b)
                             : op == 0x29u ? ps2_vu_fmac::addf(s.acc[l], ps2_vu_fmac::mulf(a, b))
                             : op == 0x20u ? ps2_vu_fmac::addf(a, s.q)
                                           : ps2_vu_fmac::addf(a, s.i);
            if (std::memcmp(&t.vf[3][l], &want, sizeof(want)) != 0)
            {
                std::printf("op 0x%02x lane %d: got %g, per-lane reference %g\n", op, l, t.vf[3][l], want);
                ++differ;
            }
        }
    }
#if defined(_MSC_VER)
    std::printf("compiler: MSVC %d (_MSC_FULL_VER %d)\n", _MSC_VER, _MSC_FULL_VER);
#elif defined(__clang__)
    std::printf("compiler: clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    std::printf("compiler: gcc %s\n", __VERSION__);
#endif
    std::printf("lane loop probe: %s\n", differ ? "LANES DIFFER (pattern reproduced)" : "lanes independent");
    return differ ? 1 : 0;
}
