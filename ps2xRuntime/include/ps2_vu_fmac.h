#pragma once
// VU FMAC arithmetic: one contract for VU0 macro mode (COP2 in EE code) and the VU micro interpreter.
// See docs/architecture/vu-ee-arithmetic-semantics.md and docs/architecture/evidence-register.md (section 2).
//
// Contract (bit level, operands and results are PS2 floats):
//   F1 exponent 0 is zero whatever the mantissa (keeps its sign); exponent 255 is an ordinary exponent.
//   F2 a result >= 2^129 saturates to +-0x7FFFFFFF; a result below 2^-126 becomes +-0 (sign of the result).
//   A1 ADD: if both operands are non-zero and their exponents differ by 25 or more, the result is the larger
//      operand unchanged, whatever the smaller one's sign (vu/games/triace, experiment E4).
//   A2 ADD otherwise: the exact sum, truncated toward zero (triace, experiment E2). An exact zero sum is +0, except
//      -0 + -0 = -0. SUB is ADD with ft's sign flipped.
//   M1 MUL: the exact product, truncated toward zero (provisional: no hardware row isolates it).
//
// Implementations: `*Ref` is pure integer arithmetic (the reference); `add/sub/mul` build doubles from the bits,
// compute exactly in double (a sum with an exponent gap below 25 needs at most 49 bits, a product 48) and truncate
// back with integer operations; `add4/sub4/mul4` are the same lowering for four lanes in SSE2. None depends on the
// host rounding mode or FTZ/DAZ (the sign of an exact-zero sum is set explicitly, since round-downward would give
// -0). ps2x_tests compares all three on boundary, structured and random operands (PS2VuFmac tests).

#include <bit>
#include <cstdint>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON) // same intrinsic source as ps2_runtime_macros.h (AArch64 builds)
#include "sse2neon.h"
#else
#include <emmintrin.h>
#endif

// The arithmetic below relies on IEEE semantics (signed zeros, exact double results). GCC/Clang -ffast-math breaks
// the EE COP1 helpers that share this header family (measured: fpenv_flag_independence on Linux), so it is refused
// at compile time; see docs/architecture/vu-ee-arithmetic-semantics.md, section 4.
#if defined(__FAST_MATH__)
#error "PS2 arithmetic requires IEEE semantics: do not build the runtime or recompiled code with -ffast-math"
#endif

// The four-lane kernels must inline: MSVC otherwise emits a call that passes __m128i by pointer and spills xmm6+
// (measured: add4 17.8 ns as a call; see docs/architecture/benchmark-methodology.md).
#if defined(_MSC_VER)
#define PS2_VU_FMAC_FORCEINLINE __forceinline
#else
#define PS2_VU_FMAC_FORCEINLINE inline __attribute__((always_inline))
#endif

namespace ps2_vu_fmac
{
    constexpr uint32_t kSign = 0x80000000u;
    constexpr uint32_t kMax = 0x7FFFFFFFu;

    // ---------------------------------------------------------------- reference (integer only)

    // sign | biased exponent | 24-bit mantissa (hidden bit included), with F2.
    inline uint32_t packRef(uint32_t sign, int32_t exponent, uint32_t mantissa24)
    {
        if (exponent >= 256)
            return sign | kMax;
        if (exponent <= 0)
            return sign;
        return sign | (static_cast<uint32_t>(exponent) << 23) | (mantissa24 & 0x007FFFFFu);
    }

    // value * 2^(exponentOfBit0 - 150), truncated to 24 significant bits.
    inline uint32_t normalizeRef(uint32_t sign, int32_t exponentOfBit0, uint64_t value)
    {
        const int msb = 63 - std::countl_zero(value);
        const uint32_t mantissa = msb >= 23 ? static_cast<uint32_t>(value >> (msb - 23))
                                            : static_cast<uint32_t>(value << (23 - msb));
        return packRef(sign, exponentOfBit0 + msb - 23, mantissa);
    }

    inline uint32_t addRef(uint32_t a, uint32_t b)
    {
        const int32_t ea = static_cast<int32_t>((a >> 23) & 0xFFu);
        const int32_t eb = static_cast<int32_t>((b >> 23) & 0xFFu);
        if (ea == 0 && eb == 0)
            return a & b & kSign; // +0 unless both are -0
        if (ea == 0)
            return b;
        if (eb == 0)
            return a;
        uint64_t ma = (a & 0x007FFFFFu) | 0x00800000u;
        uint64_t mb = (b & 0x007FFFFFu) | 0x00800000u;
        // Order by magnitude: (exponent, mantissa).
        const bool aLarger = ea > eb || (ea == eb && ma >= mb);
        const uint32_t large = aLarger ? a : b;
        const uint32_t small = aLarger ? b : a;
        const int32_t el = aLarger ? ea : eb;
        const int32_t es = aLarger ? eb : ea;
        const uint64_t ml = aLarger ? ma : mb;
        const uint64_t ms = aLarger ? mb : ma;
        const int32_t gap = el - es;
        if (gap >= 25)
            return large;
        const uint64_t aligned = ml << gap; // <= 48 bits, in units of 2^(es - 150)
        const bool sameSign = ((large ^ small) & kSign) == 0u;
        const uint64_t sum = sameSign ? aligned + ms : aligned - ms;
        if (sum == 0u)
            return 0u;
        return normalizeRef(large & kSign, es, sum);
    }

    inline uint32_t subRef(uint32_t a, uint32_t b)
    {
        return addRef(a, b ^ kSign);
    }

    inline uint32_t mulRef(uint32_t a, uint32_t b)
    {
        const uint32_t sign = (a ^ b) & kSign;
        const int32_t ea = static_cast<int32_t>((a >> 23) & 0xFFu);
        const int32_t eb = static_cast<int32_t>((b >> 23) & 0xFFu);
        if (ea == 0 || eb == 0)
            return sign;
        const uint64_t ma = (a & 0x007FFFFFu) | 0x00800000u;
        const uint64_t mb = (b & 0x007FFFFFu) | 0x00800000u;
        // (ma * 2^(ea-150)) * (mb * 2^(eb-150)) = product * 2^((ea + eb - 150) - 150)
        return normalizeRef(sign, ea + eb - 150, ma * mb);
    }

    // ---------------------------------------------------------------- lowering (exact double arithmetic)

    // A PS2 float as a double, built from the bits (exponent 255 included; exponent 0 gives a signed zero).
    inline double toDouble(uint32_t bits)
    {
        const uint64_t sign = static_cast<uint64_t>(bits & kSign) << 32;
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if (exponent == 0u)
            return std::bit_cast<double>(sign);
        return std::bit_cast<double>(sign | (static_cast<uint64_t>(exponent + 896u) << 52) |
                                     (static_cast<uint64_t>(bits & 0x007FFFFFu) << 29));
    }

    // An exact double result back to a PS2 float, truncated toward zero, with F2.
    inline uint32_t fromDoubleTruncated(double value)
    {
        const uint64_t u = std::bit_cast<uint64_t>(value);
        const uint32_t sign = static_cast<uint32_t>(u >> 32) & kSign;
        const int32_t exponent = static_cast<int32_t>((u >> 52) & 0x7FFu) - 896;
        if ((u & 0x7FFFFFFFFFFFFFFFull) == 0u)
            return sign;
        if (exponent >= 256)
            return sign | kMax;
        if (exponent <= 0)
            return sign;
        return sign | (static_cast<uint32_t>(exponent) << 23) | (static_cast<uint32_t>(u >> 29) & 0x007FFFFFu);
    }

    inline uint32_t add(uint32_t a, uint32_t b)
    {
        const uint32_t ea = (a >> 23) & 0xFFu;
        const uint32_t eb = (b >> 23) & 0xFFu;
        if (ea != 0u && eb != 0u)
        {
            if (ea >= eb + 25u)
                return a;
            if (eb >= ea + 25u)
                return b;
        }
        // Gap below 25 (or a zero operand): the double sum is exact. An exact zero is +0 unless both are negative.
        const double sum = toDouble(a) + toDouble(b);
        return sum == 0.0 ? (a & b & kSign) : fromDoubleTruncated(sum);
    }

    inline uint32_t sub(uint32_t a, uint32_t b)
    {
        return add(a, b ^ kSign);
    }

    inline uint32_t mul(uint32_t a, uint32_t b)
    {
        return fromDoubleTruncated(toDouble(a) * toDouble(b)); // 48-bit product: exact in double
    }

    // ---------------------------------------------------------------- four lanes (SSE2), same lowering

    namespace detail
    {
        // Lanes `lo`/`hi` pairs of four PS2 floats as two pairs of doubles (exponent 0 gives a signed zero).
        PS2_VU_FMAC_FORCEINLINE __m128d toDoublePair(__m128i x, bool high)
        {
            const __m128i zero = _mm_setzero_si128();
            const __m128i isZero = _mm_cmpeq_epi32(_mm_and_si128(x, _mm_set1_epi32(0x7F800000)), zero);
            const __m128i u = high ? _mm_unpackhi_epi32(x, zero) : _mm_unpacklo_epi32(x, zero);
            const __m128i z = high ? _mm_unpackhi_epi32(isZero, isZero) : _mm_unpacklo_epi32(isZero, isZero);
            const __m128i sign = _mm_slli_epi64(_mm_and_si128(u, _mm_set1_epi64x(0x80000000ll)), 32);
            // (exponent << 23 | mantissa) << 29 puts the mantissa at bits 51..29 and the exponent at 52; rebias by 896.
            const __m128i magnitude = _mm_add_epi64(_mm_slli_epi64(_mm_and_si128(u, _mm_set1_epi64x(0x7FFFFFFFll)), 29),
                                                    _mm_set1_epi64x(896ll << 52));
            return _mm_castsi128_pd(_mm_or_si128(sign, _mm_andnot_si128(z, magnitude)));
        }

        // fromDoubleTruncated for four exact double results (lanes 0,1 in r01, 2,3 in r23).
        PS2_VU_FMAC_FORCEINLINE __m128i fromDoublesTruncated(__m128d r01, __m128d r23)
        {
            const __m128i hi = _mm_castps_si128(_mm_shuffle_ps(_mm_castpd_ps(r01), _mm_castpd_ps(r23), _MM_SHUFFLE(3, 1, 3, 1)));
            const __m128i lo = _mm_castps_si128(_mm_shuffle_ps(_mm_castpd_ps(r01), _mm_castpd_ps(r23), _MM_SHUFFLE(2, 0, 2, 0)));
            const __m128i sign = _mm_and_si128(hi, _mm_set1_epi32(static_cast<int>(kSign)));
            const __m128i exponent = _mm_sub_epi32(_mm_and_si128(_mm_srli_epi32(hi, 20), _mm_set1_epi32(0x7FF)),
                                                   _mm_set1_epi32(896));
            const __m128i mantissa = _mm_or_si128(_mm_slli_epi32(_mm_and_si128(hi, _mm_set1_epi32(0xFFFFF)), 3),
                                                  _mm_srli_epi32(lo, 29));
            const __m128i normal = _mm_or_si128(sign, _mm_or_si128(_mm_slli_epi32(exponent, 23), mantissa));
            const __m128i over = _mm_cmpgt_epi32(exponent, _mm_set1_epi32(255));
            const __m128i under = _mm_cmplt_epi32(exponent, _mm_set1_epi32(1)); // also an exact zero (exponent field 0)
            const __m128i saturated = _mm_or_si128(sign, _mm_set1_epi32(static_cast<int>(kMax)));
            const __m128i inRange = _mm_andnot_si128(_mm_or_si128(over, under), normal);
            return _mm_or_si128(inRange, _mm_or_si128(_mm_and_si128(over, saturated), _mm_and_si128(under, sign)));
        }

        // Both operands' four lanes as doubles (a01, a23, b01, b23). When no lane has exponent 0 or 255 every lane is
        // an ordinary IEEE normal, and CVTPS2PD is exact whatever MXCSR says (DAZ/FTZ touch only denormals), so the
        // common case skips the bit construction; ea/eb are the lanes' biased exponents.
        PS2_VU_FMAC_FORCEINLINE void toDoubles(__m128i a, __m128i b, __m128i ea, __m128i eb, __m128d &a01, __m128d &a23,
                                               __m128d &b01, __m128d &b23)
        {
            const __m128i zero = _mm_setzero_si128(), top = _mm_set1_epi32(0xFF);
            const __m128i special = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi32(ea, zero), _mm_cmpeq_epi32(ea, top)),
                                                 _mm_or_si128(_mm_cmpeq_epi32(eb, zero), _mm_cmpeq_epi32(eb, top)));
            if (_mm_movemask_epi8(special) == 0)
            {
                const __m128 fa = _mm_castsi128_ps(a), fb = _mm_castsi128_ps(b);
                a01 = _mm_cvtps_pd(fa);
                a23 = _mm_cvtps_pd(_mm_movehl_ps(fa, fa));
                b01 = _mm_cvtps_pd(fb);
                b23 = _mm_cvtps_pd(_mm_movehl_ps(fb, fb));
                return;
            }
            a01 = toDoublePair(a, false);
            a23 = toDoublePair(a, true);
            b01 = toDoublePair(b, false);
            b23 = toDoublePair(b, true);
        }

        PS2_VU_FMAC_FORCEINLINE __m128i exponents(__m128i x)
        {
            return _mm_and_si128(_mm_srli_epi32(x, 23), _mm_set1_epi32(0xFF));
        }

        PS2_VU_FMAC_FORCEINLINE __m128i select(__m128i mask, __m128i a, __m128i b) // mask ? a : b
        {
            return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
        }
    }

    PS2_VU_FMAC_FORCEINLINE __m128i add4(__m128i a, __m128i b)
    {
        using namespace detail;
        const __m128i ea = exponents(a), eb = exponents(b);
        const __m128i zero = _mm_setzero_si128();
        const __m128i bothNonZero = _mm_andnot_si128(_mm_or_si128(_mm_cmpeq_epi32(ea, zero), _mm_cmpeq_epi32(eb, zero)),
                                                     _mm_set1_epi32(-1));
        const __m128i gap = _mm_set1_epi32(24);
        const __m128i keepA = _mm_and_si128(bothNonZero, _mm_cmpgt_epi32(ea, _mm_add_epi32(eb, gap)));
        const __m128i keepB = _mm_and_si128(bothNonZero, _mm_cmpgt_epi32(eb, _mm_add_epi32(ea, gap)));
        __m128d a01, a23, b01, b23;
        toDoubles(a, b, ea, eb, a01, a23, b01, b23);
        const __m128d s01 = _mm_add_pd(a01, b01);
        const __m128d s23 = _mm_add_pd(a23, b23);
        // An exact-zero sum: +0 unless both operands are negative (explicit, as in add()).
        const __m128i z01 = _mm_castpd_si128(_mm_cmpeq_pd(s01, _mm_setzero_pd()));
        const __m128i z23 = _mm_castpd_si128(_mm_cmpeq_pd(s23, _mm_setzero_pd()));
        const __m128i isZeroSum = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(z01), _mm_castsi128_ps(z23), _MM_SHUFFLE(2, 0, 2, 0)));
        const __m128i zeroSum = _mm_and_si128(_mm_and_si128(a, b), _mm_set1_epi32(static_cast<int>(kSign)));
        const __m128i sum = select(isZeroSum, zeroSum, fromDoublesTruncated(s01, s23));
        return select(keepA, a, select(keepB, b, sum));
    }

    PS2_VU_FMAC_FORCEINLINE __m128i sub4(__m128i a, __m128i b)
    {
        return add4(a, _mm_xor_si128(b, _mm_set1_epi32(static_cast<int>(kSign))));
    }

    PS2_VU_FMAC_FORCEINLINE __m128i mul4(__m128i a, __m128i b)
    {
        using namespace detail;
        __m128d a01, a23, b01, b23;
        toDoubles(a, b, exponents(a), exponents(b), a01, a23, b01, b23);
        return fromDoublesTruncated(_mm_mul_pd(a01, b01), _mm_mul_pd(a23, b23));
    }

    inline float addf(float a, float b) { return std::bit_cast<float>(add(std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b))); }
    inline float subf(float a, float b) { return std::bit_cast<float>(sub(std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b))); }
    inline float mulf(float a, float b) { return std::bit_cast<float>(mul(std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b))); }
}
