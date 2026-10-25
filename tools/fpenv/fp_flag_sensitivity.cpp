// Does a PS2 arithmetic helper's result depend on the host compiler's floating-point mode?
// Build this file twice (e.g. MSVC /O2 and /O2 /fp:fast, or GCC -O2 and -O2 -ffast-math) and compare the output:
// each line is an FNV-1a hash of one helper's results over the same deterministic operands, plus a count of
// results that differ from the integer reference where one exists. Identical lines mean the helper's results did not
// change with the flag on this compiler for this corpus (not a proof); different lines mean they did. Lines marked
// [info] (the pre-Phase-5 macro path) are known to depend on the flag and are not compared.
#include <cstdint>
#include <cstdio>

#include "ps2_runtime_macros.h"

namespace
{
    struct Rng
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

    uint32_t operand(Rng &r)
    {
        // Every exponent class, with weight on normal values and on 0/1/254/255.
        const uint32_t pick = r.next() & 15u;
        uint32_t e = pick == 0 ? 0u : pick == 1 ? 255u : pick == 2 ? 254u : pick == 3 ? 1u : 60u + r.next() % 140u;
        return (r.next() & 0x807FFFFFu) | (e << 23);
    }

    struct Hash
    {
        uint64_t h = 1469598103934665603ull;
        void add(uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
            {
                h ^= (v >> (8 * i)) & 0xFFu;
                h *= 1099511628211ull;
            }
        }
    };

    float f(uint32_t b) { return std::bit_cast<float>(b); }
    uint32_t u(float x) { return std::bit_cast<uint32_t>(x); }
    uint32_t lane0(__m128 v) { alignas(16) uint32_t r[4]; _mm_store_ps(reinterpret_cast<float *>(r), v); return r[0]; }
    __m128 splat(uint32_t b) { return _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(b))); }
}

int main()
{
    constexpr int kPairs = 1000000;
    Hash add, sub, mul, cop1mul, div, sqrtH, rsqrt, madd, vadd, vmul, vaddLegacy, vmulLegacy;
    uint32_t vaddRef = 0, vmulRef = 0;
    uint32_t fcr31 = 0;
    Rng r{0x853C49E6748FEA9Bull};
    for (int i = 0; i < kPairs; ++i)
    {
        const uint32_t a = operand(r), b = operand(r), c = operand(r);
        add.add(u(ps2_fadd(f(a), f(b))));
        sub.add(u(ps2_fsub(f(a), f(b))));
        mul.add(u(ps2_fmul(f(a), f(b))));
        cop1mul.add(u(ps2_cop1_mul(fcr31, f(a), f(b))));
        div.add(u(ps2_fdiv(f(a), f(b))));
        sqrtH.add(u(ps2_fsqrt(f(a))));
        rsqrt.add(u(ps2_frsqrt(f(a), f(b))));
        madd.add(u(ps2_fmadd(f(c), f(a), f(b), false)));
        const uint32_t va = lane0(ps2_vadd(splat(a), splat(b))), vm = lane0(ps2_vmul(splat(a), splat(b)));
        vadd.add(va);
        vmul.add(vm);
        vaddRef += va != ps2_vu_fmac::addRef(a, b);
        vmulRef += vm != ps2_vu_fmac::mulRef(a, b);
        vaddLegacy.add(lane0(ps2_vadd_legacy(splat(a), splat(b))));
        vmulLegacy.add(lane0(ps2_vmul_legacy(splat(a), splat(b))));
    }
    std::printf("EE ADD.S   %016llx\n", static_cast<unsigned long long>(add.h));
    std::printf("EE SUB.S   %016llx\n", static_cast<unsigned long long>(sub.h));
    std::printf("EE MUL.S   %016llx\n", static_cast<unsigned long long>(mul.h));
    std::printf("COP1 MUL   %016llx\n", static_cast<unsigned long long>(cop1mul.h));
    std::printf("EE DIV.S   %016llx\n", static_cast<unsigned long long>(div.h));
    std::printf("EE SQRT.S  %016llx\n", static_cast<unsigned long long>(sqrtH.h));
    std::printf("EE RSQRT.S %016llx\n", static_cast<unsigned long long>(rsqrt.h));
    std::printf("EE MADD.S  %016llx\n", static_cast<unsigned long long>(madd.h));
    std::printf("VU VADD    %016llx  (differs from reference: %u)\n", static_cast<unsigned long long>(vadd.h), vaddRef);
    std::printf("VU VMUL    %016llx  (differs from reference: %u)\n", static_cast<unsigned long long>(vmul.h), vmulRef);
    std::printf("[info] VU VADD legacy %016llx\n", static_cast<unsigned long long>(vaddLegacy.h));
    std::printf("[info] VU VMUL legacy %016llx\n", static_cast<unsigned long long>(vmulLegacy.h));
    std::printf("fcr31 %08x\n", fcr31);
    return 0;
}
