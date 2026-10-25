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
// Two implementations that share no code: `*Ref` is pure integer arithmetic (the reference), `add/sub/mul` build
// doubles from the bits, compute exactly in double (a sum with an exponent gap below 25 needs at most 49 bits, a
// product 48) and truncate back with integer operations, so neither depends on the host rounding mode.
// ps2x_tests compares them on boundary, structured and random operands (PS2VuFmac tests).

#include <bit>
#include <cstdint>

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
        // Gap below 25 (or a zero operand): the double sum is exact.
        return fromDoubleTruncated(toDouble(a) + toDouble(b));
    }

    inline uint32_t sub(uint32_t a, uint32_t b)
    {
        return add(a, b ^ kSign);
    }

    inline uint32_t mul(uint32_t a, uint32_t b)
    {
        return fromDoubleTruncated(toDouble(a) * toDouble(b)); // 48-bit product: exact in double
    }

    inline float addf(float a, float b) { return std::bit_cast<float>(add(std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b))); }
    inline float subf(float a, float b) { return std::bit_cast<float>(sub(std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b))); }
    inline float mulf(float a, float b) { return std::bit_cast<float>(mul(std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b))); }
}
