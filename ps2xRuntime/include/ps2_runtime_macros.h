#ifndef PS2_RUNTIME_MACROS_H
#define PS2_RUNTIME_MACROS_H
#include <cstdint>
#include <cmath>
#include <cstring>
#include <bit>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX intrinsics
#endif

#include "ps2_runtime.h"
#include "ps2_vu_fmac.h"

static inline int32_t Ps2ExtractEpi32(__m128i v, int index)
{
    switch (index & 3)
    {
    case 0:
        return _mm_extract_epi32(v, 0);
    case 1:
        return _mm_extract_epi32(v, 1);
    case 2:
        return _mm_extract_epi32(v, 2);
    default:
        return _mm_extract_epi32(v, 3);
    }
}

static inline int64_t Ps2ExtractEpi64(__m128i v, int index)
{
    if ((index & 1) == 0)
    {
        return _mm_cvtsi128_si64(v);
    }
    else
    {
        return _mm_extract_epi64(v, 1);
    }
}

static inline uint32_t ps2_clz32(uint32_t x)
{
    return static_cast<uint32_t>(std::countl_zero(x));
}

static inline uint64_t Ps2HiLoToU64(uint64_t hi, uint64_t lo)
{
    return ((hi & 0xFFFFFFFFull) << 32) | (lo & 0xFFFFFFFFull);
}

static inline uint64_t Ps2SignExt32ToU64(uint32_t v)
{
    return (uint64_t)(int64_t)(int32_t)v;
}

// PLZCW: Count leading bits that match the sign bit, minus 1.
// For positive values: count leading zeros minus 1 (excludes sign bit).
// For negative values: count leading ones minus 1 (excludes sign bit).
// Special cases: 0x00000000 -> 31, 0xFFFFFFFF -> 31.
static inline uint32_t ps2_plzcw32(uint32_t x)
{
    if (x == 0 || x == 0xFFFFFFFF)
        return 31;
    if (x & 0x80000000u)
        x = ~x; // If sign bit set, invert to count leading ones as zeros
    return static_cast<uint32_t>(std::countl_zero(x)) - 1;
}

#define PS2_BLENDV_PS(a, b, mask) _mm_blendv_ps((a), (b), (mask))
#define PS2_MIN_EPI32(a, b) _mm_min_epi32((a), (b))
#define PS2_MAX_EPI32(a, b) _mm_max_epi32((a), (b))
#define PS2_SHUFFLE_EPI8(v, mask) _mm_shuffle_epi8((v), (mask))

#define PS2_EXTRACT_EPI32(v, i) Ps2ExtractEpi32((v), (i))
#define PS2_EXTRACT_EPI64(v, i) Ps2ExtractEpi64((v), (i))

#define PS2_EXTRACT_EPI32_0(v) Ps2ExtractEpi32((v), 0)
#define PS2_EXTRACT_EPI32_1(v) Ps2ExtractEpi32((v), 1)
#define PS2_EXTRACT_EPI32_2(v) Ps2ExtractEpi32((v), 2)
#define PS2_EXTRACT_EPI32_3(v) Ps2ExtractEpi32((v), 3)

#define PS2_EXTRACT_EPI64_0(v) Ps2ExtractEpi64((v), 0)
#define PS2_EXTRACT_EPI64_1(v) Ps2ExtractEpi64((v), 1)

// Basic MIPS arithmetic operations
// The _OV forms compute in unsigned arithmetic: a signed add that overflows is undefined behaviour in C++, and
// overflow is exactly the case they must detect. The predicates are proved equal to "the exact sum is outside the
// signed range" by tools/semantics/z3_int_semantics.py.
#define ADD32(a, b) ((uint32_t)((a) + (b)))
#define ADD32_OV(rs, rt, result32, overflow)                        \
    do                                                              \
    {                                                               \
        uint32_t _a = (uint32_t)(rs);                               \
        uint32_t _b = (uint32_t)(rt);                               \
        uint32_t _r = _a + _b;                                      \
        overflow = ((~(_a ^ _b) & (_a ^ _r)) & 0x80000000u) != 0u;  \
        result32 = _r;                                              \
    } while (0);
#define SUB32(a, b) ((uint32_t)((a) - (b)))
#define SUB32_OV(rs, rt, result32, overflow)                        \
    do                                                              \
    {                                                               \
        uint32_t _a = (uint32_t)(rs);                               \
        uint32_t _b = (uint32_t)(rt);                               \
        uint32_t _r = _a - _b;                                      \
        overflow = (((_a ^ _b) & (_a ^ _r)) & 0x80000000u) != 0u;   \
        result32 = _r;                                              \
    } while (0);
#define ADD64_OV(rs, rt, result64, overflow)                                    \
    do                                                                          \
    {                                                                           \
        uint64_t _a = (uint64_t)(rs);                                           \
        uint64_t _b = (uint64_t)(rt);                                           \
        uint64_t _r = _a + _b;                                                  \
        overflow = ((~(_a ^ _b) & (_a ^ _r)) & 0x8000000000000000ull) != 0ull;  \
        result64 = _r;                                                          \
    } while (0);
#define SUB64_OV(rs, rt, result64, overflow)                                    \
    do                                                                          \
    {                                                                           \
        uint64_t _a = (uint64_t)(rs);                                           \
        uint64_t _b = (uint64_t)(rt);                                           \
        uint64_t _r = _a - _b;                                                  \
        overflow = (((_a ^ _b) & (_a ^ _r)) & 0x8000000000000000ull) != 0ull;   \
        result64 = _r;                                                          \
    } while (0);
#define MUL32(a, b) ((uint32_t)((a) * (b)))
#define DIV32(a, b) ((uint32_t)((a) / (b)))
#define AND32(a, b) ((uint32_t)((a) & (b)))
#define OR32(a, b) ((uint32_t)((a) | (b)))
#define XOR32(a, b) ((uint32_t)((a) ^ (b)))
#define NOR32(a, b) ((uint32_t)(~((a) | (b))))
#define SLL32(a, b) ((uint32_t)((a) << (b)))
#define SRL32(a, b) ((uint32_t)((a) >> (b)))
#define SRA32(a, b) ((uint32_t)((int32_t)(a) >> (b)))
#define SLT32(a, b) ((uint32_t)((int32_t)(a) < (int32_t)(b) ? 1 : 0))
#define SLTU32(a, b) ((uint32_t)((a) < (b) ? 1 : 0))

// PS2-specific 128-bit MMI operations
#define PS2_PEXTLW(a, b) _mm_unpacklo_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUW(a, b) _mm_unpackhi_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLH(a, b) _mm_unpacklo_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUH(a, b) _mm_unpackhi_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLB(a, b) _mm_unpacklo_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUB(a, b) _mm_unpackhi_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PADDW(a, b) _mm_add_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSUBW(a, b) _mm_sub_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PMAXW(a, b) PS2_MAX_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PMINW(a, b) PS2_MIN_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PADDH(a, b) _mm_add_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PSUBH(a, b) _mm_sub_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMAXH(a, b) _mm_max_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMINH(a, b) _mm_min_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PADDB(a, b) _mm_add_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PSUBB(a, b) _mm_sub_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PAND(a, b) _mm_and_si128((__m128i)(a), (__m128i)(b))
#define PS2_POR(a, b) _mm_or_si128((__m128i)(a), (__m128i)(b))
#define PS2_PXOR(a, b) _mm_xor_si128((__m128i)(a), (__m128i)(b))
#define PS2_PNOR(a, b) _mm_xor_si128(_mm_or_si128((__m128i)(a), (__m128i)(b)), _mm_set1_epi32(0xFFFFFFFF))

// PS2 float semantics (EE FPU and VU0 macro mode). The PS2 format has no Inf, NaN or denormals:
//   - exponent 255 is an ordinary exponent, so 0x7F800000 is 2^128 and the largest value is 0x7FFFFFFF;
//   - exponent 0 is zero whatever the mantissa holds;
//   - a result of 2^129 or more saturates to +-0x7FFFFFFF, one below 2^-126 becomes +-0;
//   - x/0 gives +-0x7FFFFFFF.
// A host float cannot hold the exponent-255 values, so anything that touches them is computed in double.
// That path is rare and kept out of line; the inline part is the host operation and a range check.
// One case is left to the host: ADD.S/SUB.S/MUL.S and the vector operations do not look for an operand with
// exponent 0 and a non-zero mantissa, so it takes part with its tiny host value instead of as zero.
// Checked against the hardware results in ps2autotests (tests/cpu/ee_fpu).
#if defined(_MSC_VER)
#define PS2_FLOAT_NOINLINE __declspec(noinline)
#else
#define PS2_FLOAT_NOINLINE __attribute__((noinline))
#endif

constexpr uint32_t PS2_FLOAT_MAX_BITS = 0x7FFFFFFFu;
constexpr uint32_t PS2_FLOAT_EXP_MASK = 0x7F800000u;
constexpr uint32_t PS2_FLOAT_SIGN_MASK = 0x80000000u;

// True when a host float operation cannot be trusted with x: exponent 255, or exponent 0 with a mantissa.
inline bool ps2_fspecial(float x)
{
    const uint32_t t = std::bit_cast<uint32_t>(x) & 0x7FFFFFFFu;
    return t != 0u && (t - 0x00800000u) >= 0x7F000000u;
}

inline double ps2_to_double(float x)
{
    uint32_t b = std::bit_cast<uint32_t>(x);
    const uint32_t exponent = b & PS2_FLOAT_EXP_MASK;
    if (exponent == 0u)
        return static_cast<double>(std::bit_cast<float>(b & PS2_FLOAT_SIGN_MASK));
    if (exponent == PS2_FLOAT_EXP_MASK)
        return static_cast<double>(std::bit_cast<float>(b - 0x00800000u)) * 2.0;
    return static_cast<double>(x);
}

inline float ps2_from_double(double d)
{
    const double magnitude = std::fabs(d);
    const uint32_t sign = std::signbit(d) ? PS2_FLOAT_SIGN_MASK : 0u;
    if (magnitude >= 0x1p129)
        return std::bit_cast<float>(sign | PS2_FLOAT_MAX_BITS);
    if (magnitude < 0x1p-126)
        return std::bit_cast<float>(sign);
    if (magnitude >= 0x1p128)
    {
        // Convert one exponent lower, then put the exponent back as 255.
        const uint32_t b = std::bit_cast<uint32_t>(static_cast<float>(d * 0.5));
        if ((b & PS2_FLOAT_EXP_MASK) == PS2_FLOAT_EXP_MASK)
            return std::bit_cast<float>(sign | PS2_FLOAT_MAX_BITS);
        return std::bit_cast<float>(b + 0x00800000u);
    }
    // A value that rounds up to 2^128 comes out as the host's Inf pattern, which is 2^128 on the PS2.
    return static_cast<float>(d);
}

// The slow-path counterpart: round a double toward zero to a PS2 float (saturating, flushing like ps2_from_double).
inline float ps2_from_double_toward_zero(double d)
{
    float f = ps2_from_double(d);
    const uint32_t bits = std::bit_cast<uint32_t>(f);
    if ((bits & PS2_FLOAT_EXP_MASK) == 0u || (bits & 0x7FFFFFFFu) == PS2_FLOAT_MAX_BITS)
        return f; // zero or saturated: already toward zero
    if (std::fabs(ps2_to_double(f)) > std::fabs(d))
    {
        const uint32_t stepped = bits - 1u;
        f = (stepped & PS2_FLOAT_EXP_MASK) == 0u ? std::bit_cast<float>(stepped & PS2_FLOAT_SIGN_MASK)
                                                 : std::bit_cast<float>(stepped);
    }
    return f;
}

PS2_FLOAT_NOINLINE inline float ps2_fadd_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a + b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double_toward_zero(ps2_to_double(a) + ps2_to_double(b));
}

PS2_FLOAT_NOINLINE inline float ps2_fsub_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a - b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double_toward_zero(ps2_to_double(a) - ps2_to_double(b));
}

PS2_FLOAT_NOINLINE inline float ps2_fmul_slow(float a, float b)
{
    // Zero operands and results end up here too; the host result is right for them.
    const float r = a * b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double(ps2_to_double(a) * ps2_to_double(b));
}

// A zero divisor gives +-max with the sign of fs xor ft.
PS2_FLOAT_NOINLINE inline float ps2_fdiv_slow(float a, float b)
{
    const uint32_t ua = std::bit_cast<uint32_t>(a);
    const uint32_t ub = std::bit_cast<uint32_t>(b);
    if ((ub & PS2_FLOAT_EXP_MASK) == 0u)
        return std::bit_cast<float>(((ua ^ ub) & PS2_FLOAT_SIGN_MASK) | PS2_FLOAT_MAX_BITS);
    const float r = a / b;
    if (!(ps2_fspecial(a) | ps2_fspecial(b) | ps2_fspecial(r)))
        return r;
    return ps2_from_double(ps2_to_double(a) / ps2_to_double(b));
}

// sqrt(|ft|); the result is never negative, sqrt(-0) is +0.
PS2_FLOAT_NOINLINE inline float ps2_fsqrt_slow(float a)
{
    if ((std::bit_cast<uint32_t>(a) & PS2_FLOAT_EXP_MASK) == 0u)
        return 0.0f;
    return ps2_from_double(std::sqrt(std::fabs(ps2_to_double(a))));
}

// fs / sqrt(|ft|). The sign of ft is ignored everywhere, so a zero ft gives +-max with the sign of fs alone.
PS2_FLOAT_NOINLINE inline float ps2_frsqrt_slow(float a, float b)
{
    if ((std::bit_cast<uint32_t>(b) & PS2_FLOAT_EXP_MASK) == 0u)
        return std::bit_cast<float>((std::bit_cast<uint32_t>(a) & PS2_FLOAT_SIGN_MASK) | PS2_FLOAT_MAX_BITS);
    // The root is rounded to a float before the division, as on the hardware.
    const float root = ps2_from_double(std::sqrt(std::fabs(ps2_to_double(b))));
    return ps2_from_double(ps2_to_double(a) / static_cast<double>(root));
}

// True when a, b and the host result r are all normal numbers, so r is also the PS2 result.
inline bool ps2_fall_normal(float a, float b, float r)
{
    // Doubling drops the sign; adding one exponent step wraps exponent 255 round to 0 next to exponent 0.
    const uint32_t sa = std::bit_cast<uint32_t>(a) * 2u + 0x01000000u;
    const uint32_t sb = std::bit_cast<uint32_t>(b) * 2u + 0x01000000u;
    const uint32_t sr = std::bit_cast<uint32_t>(r) * 2u + 0x01000000u;
    uint32_t lowest = sa < sb ? sa : sb;
    lowest = lowest < sr ? lowest : sr;
    return lowest >= 0x02000000u;
}

// The same test on the result alone. An exponent-255 operand always leaves exponent 255 in the host result
// of an addition, subtraction or multiplication (Inf or NaN), so this finds those operands as well.
inline bool ps2_fnormal(float r)
{
    return (std::bit_cast<uint32_t>(r) * 2u + 0x01000000u) >= 0x02000000u;
}

// ADD.S/SUB.S round toward zero (ps2autotests cpu/ee_fpu/arithmetic.expected: "add CF_MAX_MANTISSA, CF_ONE:
// 403fffff" where round-to-nearest gives 40400000; an exact-rational round-toward-zero model reproduces all 76
// hex-operand ADD/SUB rows). The host sum r rounds to nearest; when it went away from zero, step one unit back.
// The exact sum comes from double arithmetic (exact for exponent gaps up to 29 bits; past 53 the direction can be
// lost, an accepted corner). A float TwoSum is not used: MSVC folds it at higher precision for constant operands.
// Only for normal a, b and r (the fast path); 'b' is already negated for SUB.
inline float ps2_fadd_toward_zero(float a, float b, float r)
{
    const double exact = static_cast<double>(a) + static_cast<double>(b);
    if (r == 0.0f || !(std::fabs(static_cast<double>(r)) > std::fabs(exact)))
        return r;
    const uint32_t stepped = std::bit_cast<uint32_t>(r) - 1u; // one unit toward zero in sign-magnitude
    return (stepped & PS2_FLOAT_EXP_MASK) == 0u ? std::bit_cast<float>(stepped & PS2_FLOAT_SIGN_MASK)
                                                 : std::bit_cast<float>(stepped);
}

// The operands are checked too: a denormal operand is zero on the EE but can still give the host a normal result
// (ps2autotests cpu/ee_fpu/muldiv.expected: "mul CF_GARBAGE1, CF_GARBAGE2: 80000000" with a denormal fs).
inline float ps2_fadd(float a, float b)
{
    const float r = a + b;
    return ps2_fall_normal(a, b, r) ? ps2_fadd_toward_zero(a, b, r) : ps2_fadd_slow(a, b);
}

inline float ps2_fsub(float a, float b)
{
    const float r = a - b;
    return ps2_fall_normal(a, b, r) ? ps2_fadd_toward_zero(a, -b, r) : ps2_fsub_slow(a, b);
}

// MUL.S on the EE FPU (COP1 only; VU multiplies keep their own path). The mantissas are multiplied as integers and
// the product is truncated, like the other EE FPU results. The multiplier is not commutative: 1.0 * 0x3FFFFFFF
// gives 0x3FFFFFFE but 0x3FFFFFFF * 1.0 gives 0x3FFFFFFF (ps2autotests cpu/ee_fpu/muldiv.expected, also with
// CF_MAX/CF_MIN and in MULA/MADD/MSUB). That fits a radix-4 Booth recoding of ft whose lowest digit, when
// negative (ft mantissa bit 1 set), misses its +1, so the product comes out one unit low; this minimal form and
// wider truncated-array forms all reproduce the 72 MUL/MULA hardware rows, which do not tell them apart.
// Exponent 255 is an ordinary exponent, a result past it saturates to +-max, and a zero or denormal operand or a
// result below exponent 1 gives a signed zero.
inline uint32_t ps2_ee_fmul_bits(uint32_t ua, uint32_t ub)
{
    const uint32_t sign = (ua ^ ub) & PS2_FLOAT_SIGN_MASK;
    const int32_t ea = static_cast<int32_t>((ua >> 23) & 0xFFu);
    const int32_t eb = static_cast<int32_t>((ub >> 23) & 0xFFu);
    if (ea == 0 || eb == 0)
        return sign;
    const uint64_t ma = (ua & 0x007FFFFFu) | 0x00800000u;
    const uint64_t mb = (ub & 0x007FFFFFu) | 0x00800000u;
    const uint64_t product = ma * mb - ((mb >> 1) & 1u);
    int32_t exponent = ea + eb - 127;
    uint32_t mantissa;
    if ((product >> 47) != 0u)
    {
        mantissa = static_cast<uint32_t>(product >> 24);
        ++exponent;
    }
    else
    {
        mantissa = static_cast<uint32_t>(product >> 23);
    }
    if (exponent >= 256)
        return sign | PS2_FLOAT_MAX_BITS;
    if (exponent <= 0)
        return sign;
    return sign | (static_cast<uint32_t>(exponent) << 23) | (mantissa & 0x007FFFFFu);
}

inline float ps2_fmul(float a, float b)
{
    return std::bit_cast<float>(ps2_ee_fmul_bits(std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b)));
}

// COP1 forms that also update FCR31, with the flags real hardware showed (ps2autotests cpu/ee_fpu/fcr.expected,
// each starting from FCR31 = 0x01000001): a saturating ADD/SUB/MUL sets O and SO (0x8010; "NAN math" add,
// "Overflow" mul); DIV 0/0 sets I and SI (0x20040), x/0 sets D and SD (0x10020); SQRT of a negative sets I and SI.
// A DIV result flushed to zero sets nothing ("Underflow": 01000001). Only these cases are modelled.
constexpr uint32_t PS2_FCR31_OVERFLOW = 0x00008010u;
constexpr uint32_t PS2_FCR31_INVALID = 0x00020040u;
constexpr uint32_t PS2_FCR31_DIVIDE_BY_ZERO = 0x00010020u;

inline void ps2_cop1_overflow_flag(uint32_t &fcr31, double exact)
{
    if (std::fabs(exact) >= 0x1p129)
        fcr31 |= PS2_FCR31_OVERFLOW;
}

inline float ps2_cop1_add(uint32_t &fcr31, float a, float b)
{
    const float r = a + b;
    if (ps2_fall_normal(a, b, r))
        return ps2_fadd_toward_zero(a, b, r);
    ps2_cop1_overflow_flag(fcr31, ps2_to_double(a) + ps2_to_double(b));
    return ps2_fadd_slow(a, b);
}

inline float ps2_cop1_sub(uint32_t &fcr31, float a, float b)
{
    const float r = a - b;
    if (ps2_fall_normal(a, b, r))
        return ps2_fadd_toward_zero(a, -b, r);
    ps2_cop1_overflow_flag(fcr31, ps2_to_double(a) - ps2_to_double(b));
    return ps2_fsub_slow(a, b);
}

inline float ps2_cop1_mul(uint32_t &fcr31, float a, float b)
{
    const uint32_t r = ps2_ee_fmul_bits(std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b));
    if ((r & 0x7FFFFFFFu) == PS2_FLOAT_MAX_BITS)
        ps2_cop1_overflow_flag(fcr31, ps2_to_double(a) * ps2_to_double(b));
    return std::bit_cast<float>(r);
}

inline void ps2_cop1_div_flags(uint32_t &fcr31, float a, float b)
{
    if ((std::bit_cast<uint32_t>(b) & PS2_FLOAT_EXP_MASK) != 0u)
        return;
    fcr31 |= (std::bit_cast<uint32_t>(a) & PS2_FLOAT_EXP_MASK) == 0u ? PS2_FCR31_INVALID : PS2_FCR31_DIVIDE_BY_ZERO;
}

inline void ps2_cop1_sqrt_flags(uint32_t &fcr31, float a)
{
    const uint32_t bits = std::bit_cast<uint32_t>(a);
    if ((bits & PS2_FLOAT_SIGN_MASK) != 0u && (bits & PS2_FLOAT_EXP_MASK) != 0u)
        fcr31 |= PS2_FCR31_INVALID;
}

// MADD/MSUB (and the ACC forms): the product is rounded like MUL.S, except that a product past the float range is
// not saturated before the accumulation, so it outweighs the accumulator (muldiv.expected: "madd (ffffffff)
// 7fffffff, 7fffffff: 7fffffff", "msub (7f800000) 7f800000, 7f800000: ffffffff").
inline float ps2_fmadd(float acc, float a, float b, bool subtract)
{
    const double product = ps2_to_double(a) * ps2_to_double(b);
    if (std::fabs(product) >= 0x1p129)
        return ps2_from_double(subtract ? ps2_to_double(acc) - product : ps2_to_double(acc) + product);
    const float p = ps2_fmul(a, b);
    return subtract ? ps2_fsub(acc, p) : ps2_fadd(acc, p);
}

inline float ps2_fdiv(float a, float b)
{
    const float r = a / b;
    return ps2_fall_normal(a, b, r) ? r : ps2_fdiv_slow(a, b);
}

inline float ps2_fsqrt(float a)
{
    const float r = std::sqrt(a);
    return ps2_fall_normal(a, a, r) ? r : ps2_fsqrt_slow(a);
}

inline float ps2_frsqrt(float a, float b)
{
    const float r = a / std::sqrt(b);
    return ps2_fall_normal(a, b, r) ? r : ps2_frsqrt_slow(a, b);
}

enum class Ps2VecOp
{
    Add,
    Sub,
    Mul
};

// Lane by lane through the scalar helpers; only reached when a result lane has exponent 255.
PS2_FLOAT_NOINLINE inline __m128 ps2_vslow(__m128 a, __m128 b, Ps2VecOp op)
{
    alignas(16) float la[4], lb[4], lr[4];
    _mm_store_ps(la, a);
    _mm_store_ps(lb, b);
    for (int i = 0; i < 4; ++i)
    {
        switch (op)
        {
        case Ps2VecOp::Add:
            lr[i] = ps2_fadd_slow(la[i], lb[i]);
            break;
        case Ps2VecOp::Sub:
            lr[i] = ps2_fsub_slow(la[i], lb[i]);
            break;
        default:
            lr[i] = ps2_fmul_slow(la[i], lb[i]);
            break;
        }
    }
    return _mm_load_ps(lr);
}

// r is the host result of a op b. As in ps2_fnormal, testing r alone finds every lane that needs the double
// path. Results with exponent 0 are flushed to +-0.
inline __m128 ps2_vfinish(__m128 a, __m128 b, __m128 r, Ps2VecOp op)
{
    const __m128i expMask = _mm_set1_epi32(0x7F800000);
    const __m128i rexp = _mm_and_si128(_mm_castps_si128(r), expMask);
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(rexp, expMask)) != 0)
        return ps2_vslow(a, b, op);
    const __m128i flush = _mm_and_si128(_mm_cmpeq_epi32(rexp, _mm_setzero_si128()), _mm_set1_epi32(0x7FFFFFFF));
    return _mm_castsi128_ps(_mm_andnot_si128(flush, _mm_castps_si128(r)));
}

// Pre-Phase-5 macro-mode arithmetic (host round-to-nearest, no exponent-gap rule, EE-style exponent 255). Kept only
// to measure against the FMAC contract (PS2VuFmac tests, benchmarks); generated code no longer uses it.
inline __m128 ps2_vadd_legacy(__m128 a, __m128 b)
{
    return ps2_vfinish(a, b, _mm_add_ps(a, b), Ps2VecOp::Add);
}

inline __m128 ps2_vsub_legacy(__m128 a, __m128 b)
{
    return ps2_vfinish(a, b, _mm_sub_ps(a, b), Ps2VecOp::Sub);
}

inline __m128 ps2_vmul_legacy(__m128 a, __m128 b)
{
    return ps2_vfinish(a, b, _mm_mul_ps(a, b), Ps2VecOp::Mul);
}

// VU0 macro mode executes on VU0's FMAC, so COP2 VADD/VSUB/VMUL (and every macro op built on them) follow the same
// contract as the micro interpreter: ps2_vu_fmac.h (truncation, exponent-gap rule, PS2 float range).
inline __m128 ps2_vadd(__m128 a, __m128 b)
{
    return _mm_castsi128_ps(ps2_vu_fmac::add4(_mm_castps_si128(a), _mm_castps_si128(b)));
}

inline __m128 ps2_vsub(__m128 a, __m128 b)
{
    return _mm_castsi128_ps(ps2_vu_fmac::sub4(_mm_castps_si128(a), _mm_castps_si128(b)));
}

inline __m128 ps2_vmul(__m128 a, __m128 b)
{
    return _mm_castsi128_ps(ps2_vu_fmac::mul4(_mm_castps_si128(a), _mm_castps_si128(b)));
}

// PS2 VU (Vector Unit) operations
#define PS2_VADD(a, b) ps2_vadd((__m128)(a), (__m128)(b))
#define PS2_VSUB(a, b) ps2_vsub((__m128)(a), (__m128)(b))
#define PS2_VMUL(a, b) ps2_vmul((__m128)(a), (__m128)(b))
#define PS2_VDIV(a, b) _mm_div_ps((__m128)(a), (__m128)(b))
#define PS2_VMULQ(a, q) _mm_mul_ps((__m128)(a), _mm_set1_ps(q))
#define PS2_VBLEND(a, b, mask) PS2_BLENDV_PS((__m128)(a), (__m128)(b), (__m128)(mask))

// Memory access helpers - Hybrid Fast/Slow Path
// Fast path: Direct RDRAM access (masked).
// Slow path: Full runtime->Load/Store

static inline bool Ps2FastRangeIsContiguous(uint32_t offset, uint32_t bytes)
{
    return offset <= (PS2_RAM_SIZE - bytes);
}

static inline uint8_t Ps2FastRead8(const uint8_t *rdram, uint32_t addr)
{
    return rdram[addr & PS2_RAM_MASK];
}

static inline uint16_t Ps2FastRead16(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint16_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint16_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline uint32_t Ps2FastRead32(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint32_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint32_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline uint64_t Ps2FastRead64(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint64_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint64_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

// LQ/SQ/LQC2/SQC2 ignore the low four address bits: the EE aligns quadword accesses silently
// (PCSX2 R5900OpcodeImpl.cpp: memRead128(addr & ~0xf)).
static inline __m128i Ps2FastRead128(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK & ~0xFu;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        __m128i value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    __m128i value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static inline void Ps2FastWrite8(uint8_t *rdram, uint32_t addr, uint8_t value)
{
    rdram[addr & PS2_RAM_MASK] = value;
}

static inline void Ps2FastWrite16(uint8_t *rdram, uint32_t addr, uint16_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite32(uint8_t *rdram, uint32_t addr, uint32_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite64(uint8_t *rdram, uint32_t addr, uint64_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static inline void Ps2FastWrite128(uint8_t *rdram, uint32_t addr, __m128i value)
{
    const uint32_t offset = addr & PS2_RAM_MASK & ~0xFu;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

#define FAST_READ8(addr) Ps2FastRead8(rdram, (uint32_t)(addr))
#define FAST_READ16(addr) Ps2FastRead16(rdram, (uint32_t)(addr))
#define FAST_READ32(addr) Ps2FastRead32(rdram, (uint32_t)(addr))
#define FAST_READ64(addr) Ps2FastRead64(rdram, (uint32_t)(addr))
#define FAST_READ128(addr) Ps2FastRead128(rdram, (uint32_t)(addr))

#define FAST_WRITE8(addr, val) Ps2FastWrite8(rdram, (uint32_t)(addr), (uint8_t)(val))
#define FAST_WRITE16(addr, val) Ps2FastWrite16(rdram, (uint32_t)(addr), (uint16_t)(val))
#define FAST_WRITE32(addr, val) Ps2FastWrite32(rdram, (uint32_t)(addr), (uint32_t)(val))
#define FAST_WRITE64(addr, val) Ps2FastWrite64(rdram, (uint32_t)(addr), (uint64_t)(val))
#define FAST_WRITE128(addr, val) Ps2FastWrite128(rdram, (uint32_t)(addr), (val))

#define READ8(addr) ([&]() -> uint8_t {                       \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load8(rdram, ctx, _addr)                   \
        : FAST_READ8(_addr); }())

#define READ16(addr) ([&]() -> uint16_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load16(rdram, ctx, _addr)                  \
        : FAST_READ16(_addr); }())

#define READ32(addr) ([&]() -> uint32_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load32(rdram, ctx, _addr)                  \
        : FAST_READ32(_addr); }())

#define READ64(addr) ([&]() -> uint64_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load64(rdram, ctx, _addr)                  \
        : FAST_READ64(_addr); }())

#define READ128(addr) ([&]() -> __m128i {                     \
    uint32_t _addr = (uint32_t)(addr) & ~0xFu;                \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load128(rdram, ctx, _addr)                 \
        : FAST_READ128(_addr); }())

#define WRITE8(addr, val)                                                            \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store8(rdram, ctx, _addr, (val));                               \
        else                                                                         \
        {                                                                            \
            ps2TraceGuestWrite(rdram, _addr, 1u, (uint8_t)(val), 0u, "WRITE8", ctx); \
            FAST_WRITE8(_addr, (val));                                               \
        }                                                                            \
    } while (0)

#define WRITE16(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store16(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 2u, (uint16_t)(val), 0u, "WRITE16", ctx); \
            FAST_WRITE16(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE32(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store32(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 4u, (uint32_t)(val), 0u, "WRITE32", ctx); \
            FAST_WRITE32(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE64(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store64(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 8u, (uint64_t)(val), 0u, "WRITE64", ctx); \
            FAST_WRITE64(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE128(addr, val)                                                          \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (uint32_t)(addr) & ~0xFu;                                   \
        __m128i _value = (val);                                                      \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store128(rdram, ctx, _addr, _value);                            \
        else                                                                         \
        {                                                                            \
            const uint64_t _lo = static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(_value)); \
            const uint64_t _hi = static_cast<uint64_t>(PS2_EXTRACT_EPI64_1(_value)); \
            ps2TraceGuestWrite(rdram, _addr, 16u, _lo, _hi, "WRITE128", ctx);        \
            FAST_WRITE128(_addr, _value);                                            \
        }                                                                            \
    } while (0)

// Packed Compare Greater Than (PCGT)
#define PS2_PCGTW(a, b) _mm_cmpgt_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCGTH(a, b) _mm_cmpgt_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCGTB(a, b) _mm_cmpgt_epi8((__m128i)(a), (__m128i)(b))

// Packed Add with Signed Saturation Word (PADDSW)
inline __m128i ps2_paddsw(__m128i a, __m128i b)
{

    __m128i sum = _mm_add_epi32(a, b);
    // Check for over/underflow. Clamp to either INT32_MIN/INT32_MAX.
    __m128i overflow = _mm_and_si128(_mm_xor_si128(a, sum),
                                     _mm_xor_si128(b, sum));
    // Extract input sign.
    overflow = _mm_srai_epi32(overflow, 31);
    __m128i input_sign = _mm_srai_epi32(a, 31);
    // Select saturation value based on overflow sign.
    #if defined(__SSE4_1__)
    __m128i sat = _mm_blendv_epi8(
        _mm_set1_epi32(INT32_MAX),
        _mm_set1_epi32(INT32_MIN),
        input_sign);
    return _mm_blendv_epi8(sum, sat, overflow);
    #else
    __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                               _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat),
                        _mm_andnot_si128(overflow, sum));
    #endif
}
#define PS2_PADDSW(a, b) ps2_paddsw((__m128i)(a), (__m128i)(b))

// Packed Subtract with Signed Saturation Word (PSUBSW)
inline __m128i ps2_psubsw(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Check for over/underflow. Clamp to either INT32_MIN/INT32_MAX.
    __m128i overflow = _mm_and_si128(_mm_xor_si128(a, b),
                                     _mm_xor_si128(a, diff));
    // Extract input sign.
    overflow = _mm_srai_epi32(overflow, 31);
    __m128i input_sign = _mm_srai_epi32(a, 31);
    // Select saturation value based on overflow sign.
    #if defined(__SSE4_1__)
    __m128i sat = _mm_blendv_epi8(
        _mm_set1_epi32(INT32_MAX),
        _mm_set1_epi32(INT32_MIN),
        input_sign);
    return _mm_blendv_epi8(diff, sat, overflow);
    #else
    __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                               _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat),
                        _mm_andnot_si128(overflow, diff));
    #endif

}
#define PS2_PSUBSW(a, b) ps2_psubsw((__m128i)(a), (__m128i)(b))

// Packed Compare Equal (PCEQ)
#define PS2_PCEQW(a, b) _mm_cmpeq_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCEQH(a, b) _mm_cmpeq_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCEQB(a, b) _mm_cmpeq_epi8((__m128i)(a), (__m128i)(b))

// Packed Absolute (PABS)
// The most negative lane saturates to the most positive (ps2autotests cpu/ee_simd/arithmetic: pabsh 8000 -> 7fff).
#define PS2_PABSW(a) _mm_xor_si128(_mm_abs_epi32((__m128i)(a)), _mm_cmpeq_epi32((__m128i)(a), _mm_set1_epi32(INT32_MIN)))
#define PS2_PABSH(a) _mm_xor_si128(_mm_abs_epi16((__m128i)(a)), _mm_cmpeq_epi16((__m128i)(a), _mm_set1_epi16(INT16_MIN)))
#define PS2_PABSB(a) _mm_abs_epi8((__m128i)(a))

// Packed Pack (PPAC) - Packs larger elements into smaller ones
inline __m128i ps2_paddu32(__m128i a, __m128i b)
{
    __m128i sum = _mm_add_epi32(a, b);
    __m128i overflow = _mm_cmpgt_epi32(_mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)),
                                       _mm_xor_si128(sum, _mm_set1_epi32(INT32_MIN)));
    return _mm_or_si128(sum, overflow); // overflow lanes become all-1s
}
inline __m128i ps2_psubu32(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Underflow if a < b (unsigned). Clamp to 0.
    __m128i underflow = _mm_cmpgt_epi32(_mm_xor_si128(b, _mm_set1_epi32(INT32_MIN)),
                                        _mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)));
    return _mm_andnot_si128(underflow, diff); // underflow lanes become 0
}

inline __m128i ps2_ppacw(__m128i rs, __m128i rt)
{
    // rs = [rs3 rs2 rs1 rs0], rt = [rt3 rt2 rt1 rt0]
    return _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(rt), _mm_castsi128_ps(rs), _MM_SHUFFLE(2, 0, 2, 0)));
}
#define PS2_PPACW(a, b) ps2_ppacw((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppach(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 1, 4, 5, 8, 9, 12, 13,  // from rt: halfwords 0,2,4,6
        0, 1, 4, 5, 8, 9, 12, 13); // from rs: halfwords 0,2,4,6
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACH(a, b) ps2_ppach((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppacb(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 2, 4, 6, 8, 10, 12, 14,  // from rt: bytes 0,2,4,6,8,10,12,14
        0, 2, 4, 6, 8, 10, 12, 14); // from rs
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACB(a, b) ps2_ppacb((__m128i)(a), (__m128i)(b))

// Packed Interleave (PINT)
// PINTH rd, rs, rt: rd.h[2i] = rt.h[i], rd.h[2i+1] = rs.h[4+i] (the upper doubleword of rs).
// PINTEH: every word becomes (rs.h_even << 16) | rt.h_even. Both checked against ps2autotests cpu/ee_simd/shuffle.
#define PS2_PINTH(a, b) _mm_unpacklo_epi16((__m128i)(b), _mm_srli_si128((__m128i)(a), 8))
#define PS2_PINTEH(a, b) _mm_or_si128(_mm_and_si128((__m128i)(b), _mm_set1_epi32(0xFFFF)), _mm_slli_epi32((__m128i)(a), 16))

// Packed Multiply-Add (PMADD)
#define PS2_PMADDW(a, b) _mm_add_epi32(_mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(1, 0, 3, 2)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(1, 0, 3, 2))), _mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0))))

// Packed Variable Shifts
// PSLLVW/PSRLVW/PSRAVW rd, rt, rs: only words 0 and 2 shift, by rs.w & 31, and each 32-bit result is
// sign-extended into its doubleword (ps2autotests cpu/ee_simd/logic: "psllvw -1, 1: ffffffff fffffffe ...").
enum class Ps2VariableShift
{
    Left,
    RightLogical,
    RightArithmetic,
};

inline __m128i ps2PackedVariableShift(__m128i rt, __m128i rs, Ps2VariableShift kind)
{
    alignas(16) uint32_t value[4];
    alignas(16) uint32_t count[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(value), rt);
    _mm_store_si128(reinterpret_cast<__m128i *>(count), rs);
    int64_t out[2];
    for (int i = 0; i < 2; ++i)
    {
        const uint32_t v = value[2 * i];
        const uint32_t n = count[2 * i] & 31u;
        uint32_t r = 0;
        switch (kind)
        {
        case Ps2VariableShift::Left:
            r = v << n;
            break;
        case Ps2VariableShift::RightLogical:
            r = v >> n;
            break;
        case Ps2VariableShift::RightArithmetic:
            r = static_cast<uint32_t>(static_cast<int32_t>(v) >> n);
            break;
        }
        out[i] = static_cast<int32_t>(r);
    }
    return _mm_set_epi64x(out[1], out[0]);
}

#define PS2_PSLLVW(rt, rs) ps2PackedVariableShift((__m128i)(rt), (__m128i)(rs), Ps2VariableShift::Left)
#define PS2_PSRLVW(rt, rs) ps2PackedVariableShift((__m128i)(rt), (__m128i)(rs), Ps2VariableShift::RightLogical)
#define PS2_PSRAVW(rt, rs) ps2PackedVariableShift((__m128i)(rt), (__m128i)(rs), Ps2VariableShift::RightArithmetic)

inline __m128i _mm_custom_sllv_epi32(__m128i a, __m128i count)
{
    alignas(16) int32_t a_arr[4];
    alignas(16) int32_t count_arr[4];
    alignas(16) int32_t result[4];

    std::memcpy(a_arr, &a, sizeof(a));
    std::memcpy(count_arr, &count, sizeof(count));

    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] << (count_arr[i] & 0x1F);
    }

    __m128i out;
    std::memcpy(&out, result, sizeof(out));
    return out;
}

inline __m128i _mm_custom_srlv_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = (uint32_t)a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

inline __m128i _mm_custom_srav_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

// R5900 32-bit divide results, including the cases the host would trap on: x / 0 gives a quotient of -1 for x >= 0
// and 1 for x < 0 with remainder x; INT32_MIN / -1 gives INT32_MIN remainder 0 (EE DIV/DIVU semantics).
inline void ps2DivideS32(int32_t dividend, int32_t divisor, int32_t &quotient, int32_t &remainder)
{
    if (divisor == 0)
    {
        quotient = dividend < 0 ? 1 : -1;
        remainder = dividend;
    }
    else if (divisor == -1 && dividend == INT32_MIN)
    {
        quotient = INT32_MIN;
        remainder = 0;
    }
    else
    {
        quotient = dividend / divisor;
        remainder = dividend % divisor;
    }
}

// PDIVW / PDIVUW: words 0 and 2 of rs divided by the same words of rt; LO/HI doubleword i gets the sign-extended
// quotient/remainder. PDIVBW: every word of rs divided by the halfword rt.h0; LO/HI word i gets quotient/remainder.
enum class Ps2PackedDivide
{
    Words,
    UnsignedWords,
    WordsByHalfword,
};

// PMFHL function implementations
inline __m128i ps2_u64_to_epi64_pair(uint64_t value)
{
    return _mm_set1_epi64x(static_cast<long long>(value));
}

#define PS2_PMFHL_LW(hi, lo) _mm_unpacklo_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_UW(hi, lo) _mm_unpackhi_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_SLW(hi, lo) _mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_LH(hi, lo) _mm_shuffle_epi32(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0))
#define PS2_PMFHL_SH(hi, lo) _mm_shufflehi_epi16(_mm_shufflelo_epi16(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0)), _MM_SHUFFLE(3, 1, 2, 0))

// FPU (COP1) operations
#define FPU_SET_ACC(ctx, res) (ctx->f_acc = res)
#define FPU_ADD_S(a, b) ps2_fadd((float)(a), (float)(b))
#define FPU_SUB_S(a, b) ps2_fsub((float)(a), (float)(b))
#define FPU_MUL_S(a, b) ps2_fmul((float)(a), (float)(b))
#define FPU_DIV_S(a, b) ps2_fdiv((float)(a), (float)(b))
#define FPU_RSQRT_S(a, b) ps2_frsqrt((float)(a), (float)(b))
#define FPU_SQRT_S(a) ps2_fsqrt((float)(a))
#define FPU_ABS_S(a) fabsf((float)(a))
#define FPU_MOV_S(a) ((float)(a))
#define FPU_NEG_S(a) (-(float)(a))
#define FPU_ROUND_L_S(a) ((int64_t)roundf((float)(a)))
#define FPU_TRUNC_L_S(a) ((int64_t)(float)(a))
#define FPU_CEIL_L_S(a) ((int64_t)ceilf((float)(a)))
#define FPU_FLOOR_L_S(a) ((int64_t)floorf((float)(a)))
#define FPU_ROUND_W_S(a) ((int32_t)nearbyintf((float)(a)))
#define FPU_TRUNC_W_S(a) ((int32_t)(float)(a))
#define FPU_CEIL_W_S(a) ((int32_t)ceilf((float)(a)))
#define FPU_FLOOR_W_S(a) ((int32_t)floorf((float)(a)))
// CVT.S.W rounds toward zero on the EE (ps2autotests cpu/ee_fpu/convert: 0x7FFFFFFF -> 0x4EFFFFFF, where the host's
// round-to-nearest gives 0x4F000000): drop the bits below the 24-bit significand before the exact conversion.
inline float ps2_cvt_s_w(int32_t value)
{
    uint32_t magnitude = value < 0 ? 0u - static_cast<uint32_t>(value) : static_cast<uint32_t>(value);
    int width = 0;
    for (uint32_t m = magnitude; m != 0u; m >>= 1)
        ++width;
    if (width > 24)
        magnitude &= ~((1u << (width - 24)) - 1u);
    const float result = static_cast<float>(magnitude);
    return value < 0 ? -result : result;
}
#define FPU_CVT_S_W(a) ps2_cvt_s_w((int32_t)(a))
#define FPU_CVT_S_L(a) ((float)(int64_t)(a))
// CVT.W.S truncates toward zero; a magnitude of 2^31 or more saturates by sign (PCSX2 FPU.cpp CVT_W).
inline int32_t ps2_fcvt_w_s(float a)
{
    const uint32_t bits = std::bit_cast<uint32_t>(a);
    if ((bits & PS2_FLOAT_EXP_MASK) <= 0x4E800000u)
        return static_cast<int32_t>(a);
    return (bits & PS2_FLOAT_SIGN_MASK) ? INT32_MIN : INT32_MAX;
}
#define FPU_CVT_W_S(a) ps2_fcvt_w_s((float)(a))

// VU0 macro-mode Q-unit operations (DIV/SQRT/RSQRT). They clear and set the status I (0x10) and D (0x20) flags
// and their sticky copies IS (0x400) and DS (0x800). Semantics follow PCSX2 VUops.cpp _vuDIV/_vuSQRT/_vuRSQRT;
// the +-max result is the PS2's 0x7FFFFFFF used by the EE FPU semantics above, where PCSX2 clamps to 0x7F7FFFFF.
constexpr uint16_t PS2_VU_STATUS_I = 0x0010u;
constexpr uint16_t PS2_VU_STATUS_D = 0x0020u;
constexpr uint16_t PS2_VU_STATUS_IS = 0x0400u;
constexpr uint16_t PS2_VU_STATUS_DS = 0x0800u;

inline bool ps2_fis_zero(float x)
{
    return (std::bit_cast<uint32_t>(x) & PS2_FLOAT_EXP_MASK) == 0u;
}

inline bool ps2_fis_negative_nonzero(float x)
{
    return !ps2_fis_zero(x) && (std::bit_cast<uint32_t>(x) & PS2_FLOAT_SIGN_MASK) != 0u;
}

inline void ps2_vu_raise(uint16_t &status, uint16_t flag)
{
    status = static_cast<uint16_t>(status | flag | (flag == PS2_VU_STATUS_I ? PS2_VU_STATUS_IS : PS2_VU_STATUS_DS));
}

inline float ps2_vu_signed_max(float a, float b)
{
    const uint32_t sign = (std::bit_cast<uint32_t>(a) ^ std::bit_cast<uint32_t>(b)) & PS2_FLOAT_SIGN_MASK;
    return std::bit_cast<float>(sign | PS2_FLOAT_MAX_BITS);
}

inline float ps2_vu_div(uint16_t &status, float fs, float ft)
{
    status = static_cast<uint16_t>(status & ~(PS2_VU_STATUS_I | PS2_VU_STATUS_D));
    if (ps2_fis_zero(ft))
    {
        ps2_vu_raise(status, ps2_fis_zero(fs) ? PS2_VU_STATUS_I : PS2_VU_STATUS_D);
        return ps2_vu_signed_max(fs, ft);
    }
    return ps2_fdiv_slow(fs, ft);
}

inline float ps2_vu_sqrt(uint16_t &status, float ft)
{
    status = static_cast<uint16_t>(status & ~(PS2_VU_STATUS_I | PS2_VU_STATUS_D));
    if (ps2_fis_negative_nonzero(ft))
        ps2_vu_raise(status, PS2_VU_STATUS_I);
    return ps2_fsqrt_slow(ft);
}

inline float ps2_vu_rsqrt(uint16_t &status, float fs, float ft)
{
    status = static_cast<uint16_t>(status & ~(PS2_VU_STATUS_I | PS2_VU_STATUS_D));
    if (ps2_fis_zero(ft))
    {
        ps2_vu_raise(status, PS2_VU_STATUS_D);
        if (!ps2_fis_zero(fs))
            return ps2_vu_signed_max(fs, ft);
        ps2_vu_raise(status, PS2_VU_STATUS_I);
        return std::bit_cast<float>((std::bit_cast<uint32_t>(fs) ^ std::bit_cast<uint32_t>(ft)) & PS2_FLOAT_SIGN_MASK);
    }
    if (ps2_fis_negative_nonzero(ft))
        ps2_vu_raise(status, PS2_VU_STATUS_I);
    return ps2_frsqrt_slow(fs, ft);
}
#define FPU_CVT_L_S(a) ((int64_t)(float)(a))
#define FPU_C_F_S(a, b) (0)
#define FPU_C_UN_S(a, b) (isnan((float)(a)) || isnan((float)(b)))
// The EE FPU has no NaN or infinity and treats denormals as zero, so it compares encodings, not IEEE values
// (ps2autotests cpu/ee_fpu/compare.expected: "eq 7fffffff, 7fffffff: true", "eq 00000000, 00000001: true",
// "lt ffffffff, 7fffffff: true"). Zero and denormal of either sign compare equal; the rest is sign-magnitude.
inline uint32_t ps2FloatBits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline int32_t ps2FpuCompareKey(float value)
{
    const uint32_t bits = ps2FloatBits(value);
    if ((bits & 0x7F800000u) == 0u)
        return 0;
    const int32_t magnitude = static_cast<int32_t>(bits & 0x7FFFFFFFu);
    return (bits & 0x80000000u) ? -magnitude : magnitude;
}

// MAX.S/MIN.S order encodings the same way but keep -0 below +0 (cpu/ee_fpu/arithmetic.expected:
// "max 80000000, 00000000: 00000000", "max 7fffffff, ffffffff: 7fffffff").
inline int64_t ps2FpuMinMaxKey(float value)
{
    const uint32_t bits = ps2FloatBits(value);
    const int64_t magnitude = static_cast<int64_t>(bits & 0x7FFFFFFFu);
    return (bits & 0x80000000u) ? -magnitude - 1 : magnitude;
}

inline float ps2FpuMax(float a, float b) { return ps2FpuMinMaxKey(a) >= ps2FpuMinMaxKey(b) ? a : b; }
inline float ps2FpuMin(float a, float b) { return ps2FpuMinMaxKey(a) <= ps2FpuMinMaxKey(b) ? a : b; }

#define FPU_C_EQ_S(a, b) (ps2FpuCompareKey((float)(a)) == ps2FpuCompareKey((float)(b)))
#define FPU_C_UEQ_S(a, b) ((float)(a) == (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_OLT_S(a, b) (ps2FpuCompareKey((float)(a)) < ps2FpuCompareKey((float)(b))) // R5900 C.LT
#define FPU_C_ULT_S(a, b) ((float)(a) < (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_OLE_S(a, b) (ps2FpuCompareKey((float)(a)) <= ps2FpuCompareKey((float)(b))) // R5900 C.LE
#define FPU_C_ULE_S(a, b) ((float)(a) <= (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_SF_S(a, b) (0)
#define FPU_C_NGLE_S(a, b) (isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_SEQ_S(a, b) ((float)(a) == (float)(b))
#define FPU_C_NGL_S(a, b) ((float)(a) == (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_LT_S(a, b) ((float)(a) < (float)(b))
#define FPU_C_NGE_S(a, b) ((float)(a) < (float)(b) || isnan((float)(a)) || isnan((float)(b)))
#define FPU_C_LE_S(a, b) ((float)(a) <= (float)(b))
#define FPU_C_NGT_S(a, b) ((float)(a) <= (float)(b) || isnan((float)(a)) || isnan((float)(b)))

// QFSRV: Quadword Funnel Shift Right Variable
// Concatenates rs || rt (256 bits) and right-shifts by SA bits, taking lower 128 bits.
inline __m128i ps2_qfsrv(__m128i rs, __m128i rt, uint32_t sa)
{
    if (sa == 0)
        return rt;
    if (sa >= 128)
    {
        if (sa >= 256)
            return _mm_setzero_si128();
        uint32_t shift = sa - 128;
        if (shift == 0)
            return rs;
        // Shift rs right by (sa-128) bits
        uint32_t byteShift = shift / 8;
        uint32_t bitShift = shift % 8;
        // Byte shift rs right
        alignas(16) uint8_t buf[16] = {};
        alignas(16) uint8_t src[16];
        _mm_store_si128((__m128i *)src, rs);
        for (uint32_t i = 0; i + byteShift < 16; i++)
            buf[i] = src[i + byteShift];
        __m128i result = _mm_load_si128((__m128i *)buf);
        if (bitShift > 0)
            result = _mm_or_si128(_mm_srli_epi64(result, bitShift),
                                  _mm_slli_epi64(_mm_bsrli_si128(result, 8), 64 - bitShift));
        return result;
    }
    // sa is 1..127: result = (rs || rt) >> sa, lower 128 bits
    uint32_t byteShift = sa / 8;
    uint32_t bitShift = sa % 8;
    alignas(16) uint8_t combined[32];
    _mm_store_si128((__m128i *)(combined), rt);      // low 128 bits
    _mm_store_si128((__m128i *)(combined + 16), rs); // high 128 bits
    // Shift right by byteShift bytes
    alignas(16) uint8_t shifted[16];
    for (uint32_t i = 0; i < 16; i++)
        shifted[i] = (i + byteShift < 32) ? combined[i + byteShift] : 0;
    __m128i result = _mm_load_si128((__m128i *)shifted);
    if (bitShift > 0)
    {
        uint8_t extra = (byteShift + 16 < 32) ? combined[byteShift + 16] : 0;
        __m128i hi_byte = _mm_insert_epi8(_mm_setzero_si128(), extra, 15);
        alignas(16) uint8_t src32[32];
        for (uint32_t i = 0; i < 32; i++)
            src32[i] = combined[i];
        uint64_t lo0, lo1, hi0, hi1;
        std::memcpy(&lo0, src32, 8);
        std::memcpy(&lo1, src32 + 8, 8);
        std::memcpy(&hi0, src32 + 16, 8);
        std::memcpy(&hi1, src32 + 24, 8);
        // 256-bit right shift by sa bits
        uint64_t r0, r1;
        if (sa < 64)
        {
            r0 = (lo0 >> sa) | (lo1 << (64 - sa));
            r1 = (lo1 >> sa) | (hi0 << (64 - sa));
        }
        else if (sa < 128)
        {
            uint32_t s = sa - 64;
            if (s == 0)
            {
                r0 = lo1;
                r1 = hi0;
            }
            else
            {
                r0 = (lo1 >> s) | (hi0 << (64 - s));
                r1 = (hi0 >> s) | (hi1 << (64 - s));
            }
        }
        else
        {
            r0 = 0;
            r1 = 0; // handled above
        }
        result = _mm_set_epi64x((long long)r1, (long long)r0);
    }
    return result;
}
#define PS2_QFSRV(rs, rt, sa) ps2_qfsrv((__m128i)(rs), (__m128i)(rt), (uint32_t)(sa))
#define PS2_PCPYLD(rs, rt) _mm_unpacklo_epi64(rt, rs)
// Halfword/word exchanges within each doubleword, checked against ps2autotests cpu/ee_simd/shuffle.expected.
#define PS2_HALFWORDS_PER_DOUBLEWORD(rt, order) _mm_shufflelo_epi16(_mm_shufflehi_epi16((__m128i)(rt), order), order)
#define PS2_PEXEH(rt) PS2_HALFWORDS_PER_DOUBLEWORD(rt, _MM_SHUFFLE(3, 0, 1, 2)) // h0 <-> h2
#define PS2_PEXCH(rt) PS2_HALFWORDS_PER_DOUBLEWORD(rt, _MM_SHUFFLE(3, 1, 2, 0)) // h1 <-> h2
#define PS2_PREVH(rt) PS2_HALFWORDS_PER_DOUBLEWORD(rt, _MM_SHUFFLE(0, 1, 2, 3)) // h0..h3 reversed
#define PS2_PEXCW(rt) _mm_shuffle_epi32((__m128i)(rt), _MM_SHUFFLE(3, 1, 2, 0))  // w1 <-> w2
#define PS2_PADDUH(rs, rt) _mm_adds_epu16((__m128i)(rs), (__m128i)(rt))
#define PS2_PSUBUH(rs, rt) _mm_subs_epu16((__m128i)(rs), (__m128i)(rt))
// PEXEW swaps words 0 and 2: rd = {rt2, rt1, rt0, rt3} (PCSX2 MMI.cpp PEXEW).
#define PS2_PEXEW(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(3, 0, 1, 2))
#define PS2_PROT3W(rt) _mm_shuffle_epi32((__m128i)(rt), _MM_SHUFFLE(3, 0, 2, 1)) // {w1, w2, w0, w3}

// Additional VU0 operations
#define PS2_VSQRT(x) sqrtf(x)
#define PS2_VRSQRT(x) (1.0f / sqrtf(x))

#define GPR_U32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0U : static_cast<uint32_t>(PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx])))
#define GPR_S32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0 : PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx]))
#define GPR_U64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0ULL : static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx])))
#define GPR_S64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0LL : PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx]))
#define GPR_VEC(ctx_ptr, reg_idx) ((reg_idx == 0) ? _mm_setzero_si128() : ctx_ptr->r[reg_idx])

static inline void Ps2SetGprLow64(R5900Context *ctx, int reg, __m128i new_low)
{
    if (reg != 0)
    {
        ctx->r[reg] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(ctx->r[reg]), _mm_castsi128_pd(new_low)));
    }
}

// ---- MMI HI/LO family (PCSX2 MMI.cpp). LO and HI are 128-bit on the EE: words 0-1 live in ctx->lo/hi (pipeline 0),
// words 2-3 in ctx->lo1/hi1 (pipeline 1). All halfword products are exact s32 values, one per word.
struct Ps2HiLoWords
{
    uint32_t lo[4];
    uint32_t hi[4];
};

static inline Ps2HiLoWords ps2LoadHiLo(const R5900Context *ctx)
{
    Ps2HiLoWords w;
    w.lo[0] = static_cast<uint32_t>(ctx->lo);
    w.lo[1] = static_cast<uint32_t>(ctx->lo >> 32);
    w.lo[2] = static_cast<uint32_t>(ctx->lo1);
    w.lo[3] = static_cast<uint32_t>(ctx->lo1 >> 32);
    w.hi[0] = static_cast<uint32_t>(ctx->hi);
    w.hi[1] = static_cast<uint32_t>(ctx->hi >> 32);
    w.hi[2] = static_cast<uint32_t>(ctx->hi1);
    w.hi[3] = static_cast<uint32_t>(ctx->hi1 >> 32);
    return w;
}

static inline void ps2StoreHiLo(R5900Context *ctx, const Ps2HiLoWords &w)
{
    ctx->lo = static_cast<uint64_t>(w.lo[0]) | (static_cast<uint64_t>(w.lo[1]) << 32);
    ctx->lo1 = static_cast<uint64_t>(w.lo[2]) | (static_cast<uint64_t>(w.lo[3]) << 32);
    ctx->hi = static_cast<uint64_t>(w.hi[0]) | (static_cast<uint64_t>(w.hi[1]) << 32);
    ctx->hi1 = static_cast<uint64_t>(w.hi[2]) | (static_cast<uint64_t>(w.hi[3]) << 32);
}

static inline __m128i ps2Words128(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    return _mm_set_epi32(static_cast<int32_t>(w3), static_cast<int32_t>(w2), static_cast<int32_t>(w1), static_cast<int32_t>(w0));
}

static inline void ps2PackedDivideToHiLo(R5900Context *ctx, __m128i rs, __m128i rt, Ps2PackedDivide kind)
{
    alignas(16) uint32_t a[4];
    alignas(16) uint32_t b[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(a), rs);
    _mm_store_si128(reinterpret_cast<__m128i *>(b), rt);
    Ps2HiLoWords w{};
    if (kind == Ps2PackedDivide::WordsByHalfword)
    {
        const int32_t divisor = static_cast<int16_t>(b[0] & 0xFFFFu);
        for (int i = 0; i < 4; ++i)
        {
            int32_t q = 0, r = 0;
            ps2DivideS32(static_cast<int32_t>(a[i]), divisor, q, r);
            w.lo[i] = static_cast<uint32_t>(q);
            w.hi[i] = static_cast<uint32_t>(r);
        }
        ps2StoreHiLo(ctx, w);
        return;
    }
    for (int i = 0; i < 2; ++i)
    {
        uint32_t q = 0, r = 0;
        if (kind == Ps2PackedDivide::UnsignedWords)
        {
            const uint32_t n = a[2 * i], d = b[2 * i];
            q = d != 0u ? n / d : 0xFFFFFFFFu;
            r = d != 0u ? n % d : n;
        }
        else
        {
            int32_t sq = 0, sr = 0;
            ps2DivideS32(static_cast<int32_t>(a[2 * i]), static_cast<int32_t>(b[2 * i]), sq, sr);
            q = static_cast<uint32_t>(sq);
            r = static_cast<uint32_t>(sr);
        }
        const uint32_t qHigh = (q & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        const uint32_t rHigh = (r & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        w.lo[2 * i] = q;
        w.lo[2 * i + 1] = qHigh;
        w.hi[2 * i] = r;
        w.hi[2 * i + 1] = rHigh;
    }
    ps2StoreHiLo(ctx, w);
}

enum class Ps2MmiAccumulate
{
    Set,
    Add,
    Sub
};

// PMULTH / PMADDH / PMSUBH: product i of the eight halfword pairs goes to LO word {0,1,_,_,2,3,_,_}[i] or
// HI word {_,_,0,1,_,_,2,3}[i]; rd = {LO0, HI0, LO2, HI2}.
// PMULTW/PMULTUW/PMADDW/PMADDUW/PMSUBW: words 0 and 2 of rs and rt multiply into 64-bit products. Doubleword i of
// HI:LO (HI.w[2i] above LO.w[2i]) is the accumulator; the 64-bit result r goes to rd.d[i], and LO/HI doubleword i
// get the sign-extended low/high words of r (ps2autotests cpu/ee_simd/muldiv.expected, "pmaddw 1, 1": rd
// abcdef01 cdef0124 89abcdef 9abcdef1 from C_HILO).
static inline __m128i ps2MmiWordMultiply(R5900Context *ctx, __m128i rs, __m128i rt, Ps2MmiAccumulate mode, bool isUnsigned)
{
    alignas(16) uint32_t a[4];
    alignas(16) uint32_t b[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(a), rs);
    _mm_store_si128(reinterpret_cast<__m128i *>(b), rt);
    Ps2HiLoWords w = ps2LoadHiLo(ctx);
    uint64_t out[2];
    for (int i = 0; i < 2; ++i)
    {
        const int k = 2 * i;
        const uint64_t product = isUnsigned
                                     ? static_cast<uint64_t>(a[k]) * static_cast<uint64_t>(b[k])
                                     : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a[k])) *
                                                             static_cast<int64_t>(static_cast<int32_t>(b[k])));
        const uint64_t acc = (static_cast<uint64_t>(w.hi[k]) << 32) | w.lo[k];
        const uint64_t r = mode == Ps2MmiAccumulate::Set ? product : mode == Ps2MmiAccumulate::Add ? acc + product : acc - product;
        out[i] = r;
        const uint32_t low = static_cast<uint32_t>(r);
        const uint32_t high = static_cast<uint32_t>(r >> 32);
        w.lo[k] = low;
        w.lo[k + 1] = (low & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        w.hi[k] = high;
        w.hi[k + 1] = (high & 0x80000000u) ? 0xFFFFFFFFu : 0u;
    }
    ps2StoreHiLo(ctx, w);
    return _mm_set_epi64x(static_cast<int64_t>(out[1]), static_cast<int64_t>(out[0]));
}

static inline __m128i ps2MmiHalfwordMultiply(R5900Context *ctx, __m128i rs, __m128i rt, Ps2MmiAccumulate mode)
{
    alignas(16) int16_t a[8];
    alignas(16) int16_t b[8];
    _mm_store_si128(reinterpret_cast<__m128i *>(a), rs);
    _mm_store_si128(reinterpret_cast<__m128i *>(b), rt);
    Ps2HiLoWords w = ps2LoadHiLo(ctx);
    for (int i = 0; i < 8; ++i)
    {
        const uint32_t product = static_cast<uint32_t>(static_cast<int32_t>(a[i]) * static_cast<int32_t>(b[i]));
        uint32_t &word = ((i & 2) ? w.hi : w.lo)[((i >> 2) << 1) | (i & 1)];
        word = (mode == Ps2MmiAccumulate::Set) ? product : (mode == Ps2MmiAccumulate::Add) ? word + product : word - product;
    }
    ps2StoreHiLo(ctx, w);
    return ps2Words128(w.lo[0], w.hi[0], w.lo[2], w.hi[2]);
}

// PHMADH / PHMSBH: pair k (halfwords 2k, 2k+1) gives first = p[2k+1], sum = first +/- p[2k]; pairs 0..3 go to
// LO0/1, HI0/1, LO2/3, HI2/3 as {sum, first} (PHMSBH stores ~first, PCSX2's documented-as-undocumented result).
static inline __m128i ps2MmiHorizontalMultiply(R5900Context *ctx, __m128i rs, __m128i rt, bool subtract)
{
    alignas(16) int16_t a[8];
    alignas(16) int16_t b[8];
    _mm_store_si128(reinterpret_cast<__m128i *>(a), rs);
    _mm_store_si128(reinterpret_cast<__m128i *>(b), rt);
    Ps2HiLoWords w{};
    for (int k = 0; k < 4; ++k)
    {
        const int32_t first = static_cast<int32_t>(a[2 * k + 1]) * static_cast<int32_t>(b[2 * k + 1]);
        const int32_t second = static_cast<int32_t>(a[2 * k]) * static_cast<int32_t>(b[2 * k]);
        uint32_t *reg = (k & 1) ? w.hi : w.lo;
        const int word = (k >> 1) << 1;
        reg[word] = subtract ? static_cast<uint32_t>(first) - static_cast<uint32_t>(second)
                             : static_cast<uint32_t>(first) + static_cast<uint32_t>(second);
        reg[word + 1] = subtract ? ~static_cast<uint32_t>(first) : static_cast<uint32_t>(first);
    }
    ps2StoreHiLo(ctx, w);
    return ps2Words128(w.lo[0], w.hi[0], w.lo[2], w.hi[2]);
}

static inline uint16_t ps2PmfhlClamp(int32_t v)
{
    return static_cast<uint16_t>(v > 0x7FFF ? 0x7FFF : v < -0x8000 ? 0x8000 : v);
}

static inline uint64_t ps2PmfhlSlw(uint32_t hiWord, uint32_t loWord)
{
    const int64_t v = static_cast<int64_t>((static_cast<uint64_t>(hiWord) << 32) | loWord);
    if (v >= 0x7FFFFFFFLL)
        return 0x000000007FFFFFFFull;
    if (v <= -0x80000000LL)
        return 0xFFFFFFFF80000000ull;
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(loWord)));
}

// PMFHL.fmt rd (fmt = sa: 0 LW, 1 UW, 2 SLW, 3 LH, 4 SH).
static inline __m128i ps2Pmfhl(const R5900Context *ctx, int fmt)
{
    const Ps2HiLoWords w = ps2LoadHiLo(ctx);
    switch (fmt)
    {
    case 0:
        return ps2Words128(w.lo[0], w.hi[0], w.lo[2], w.hi[2]);
    case 1:
        return ps2Words128(w.lo[1], w.hi[1], w.lo[3], w.hi[3]);
    case 2:
        return _mm_set_epi64x(static_cast<int64_t>(ps2PmfhlSlw(w.hi[2], w.lo[2])), static_cast<int64_t>(ps2PmfhlSlw(w.hi[0], w.lo[0])));
    case 3:
        return ps2Words128(static_cast<uint16_t>(w.lo[0]) | (w.lo[1] << 16), static_cast<uint16_t>(w.hi[0]) | (w.hi[1] << 16),
                           static_cast<uint16_t>(w.lo[2]) | (w.lo[3] << 16), static_cast<uint16_t>(w.hi[2]) | (w.hi[3] << 16));
    default:
    {
        auto pack = [](uint32_t a, uint32_t b)
        { return static_cast<uint32_t>(ps2PmfhlClamp(static_cast<int32_t>(a))) | (static_cast<uint32_t>(ps2PmfhlClamp(static_cast<int32_t>(b))) << 16); };
        return ps2Words128(pack(w.lo[0], w.lo[1]), pack(w.hi[0], w.hi[1]), pack(w.lo[2], w.lo[3]), pack(w.hi[2], w.hi[3]));
    }
    }
}

// PMTHL.LW rs: LO0 = rs0, HI0 = rs1, LO2 = rs2, HI2 = rs3; words 1 and 3 keep their values.
static inline void ps2PmthlLw(R5900Context *ctx, __m128i rs)
{
    alignas(16) uint32_t v[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(v), rs);
    Ps2HiLoWords w = ps2LoadHiLo(ctx);
    w.lo[0] = v[0];
    w.hi[0] = v[1];
    w.lo[2] = v[2];
    w.hi[2] = v[3];
    ps2StoreHiLo(ctx, w);
}

#define SET_GPR_U32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
                                                                          \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)


#define SET_GPR_ZE32(ctx_ptr, reg_idx, val)                               \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(uint32_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_S32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(int32_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);                    \
        }                                                                 \
    } while (0)

#define SET_GPR_U64(ctx_ptr, reg_idx, val)                       \
    do                                                           \
    {                                                            \
        if ((reg_idx) != 0)                                      \
        {                                                        \
            __m128i _newVal = _mm_cvtsi64_si128((int64_t)(val)); \
            Ps2SetGprLow64(ctx_ptr, reg_idx, _newVal);           \
        }                                                        \
    } while (0)

#define SET_GPR_S64(ctx_ptr, reg_idx, val) SET_GPR_U64(ctx_ptr, reg_idx, val)

#define SET_GPR_VEC(ctx_ptr, reg_idx, val) \
    do                                     \
    {                                      \
        if (reg_idx != 0)                  \
            ctx_ptr->r[reg_idx] = (val);   \
    } while (0)

#endif // PS2_RUNTIME_MACROS_H
