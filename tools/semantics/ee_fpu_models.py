#!/usr/bin/env python3
"""Exact (integer/rational) candidate models of EE COP1 arithmetic, for model discrimination.

This is research tooling, not the runtime: every model here is written independently of the C++ helpers, in exact
integer arithmetic, so a disagreement between a model and the runtime is evidence about one of them.

PS2 float format (established by ps2autotests cpu/ee_fpu, see docs/architecture/evidence-register.md):
  exponent 0 is zero whatever the mantissa; exponent 255 is an ordinary exponent (0x7F800000 = 2^128);
  results >= 2^129 saturate to +-0x7FFFFFFF; results below 2^-126 flush to +-0.
"""
from fractions import Fraction

SIGN = 0x80000000
MAXB = 0x7FFFFFFF


def decode(bits):
    """-> (sign, exponent, 24-bit mantissa) or None for zero."""
    e = (bits >> 23) & 0xFF
    if e == 0:
        return None
    return (bits >> 31) & 1, e, (bits & 0x7FFFFF) | 0x800000


def value(bits):
    d = decode(bits)
    if d is None:
        return Fraction(0)
    s, e, m = d
    v = Fraction(m) * Fraction(2) ** (e - 150)
    return -v if s else v


def encode_toward_zero(v, zero_sign=0):
    """Exact rational -> PS2 float, truncating toward zero, saturating and flushing."""
    if v == 0:
        return zero_sign << 31
    sign = SIGN if v < 0 else 0
    v = abs(v)
    e = v.numerator.bit_length() - v.denominator.bit_length()
    if Fraction(2) ** e > v:
        e -= 1
    if Fraction(2) ** (e + 1) <= v:
        e += 1
    be = e + 127
    if be >= 256:
        return sign | MAXB
    if be <= 0:
        return sign
    scaled = v / Fraction(2) ** (e - 23)
    m = scaled.numerator // scaled.denominator
    return sign | (be << 23) | (m & 0x7FFFFF)


# ---------------------------------------------------------------- ADD/SUB candidates

def add_exact_toward_zero(a, b):
    """M1: the exact sum, rounded toward zero (current runtime contract)."""
    va, vb = value(a), value(b)
    if va == 0 and vb == 0:
        return (a & b) & SIGN            # +0 + -0 = +0, -0 + -0 = -0 (IEEE toward-zero rule)
    if va == 0:
        return b if decode(b) else b & SIGN
    if vb == 0:
        return a
    s = va + vb
    return encode_toward_zero(s, zero_sign=0)


def add_aligned(a, b, guard_bits):
    """M2(g): align the smaller mantissa by the exponent gap keeping `guard_bits` extra bits (the rest is dropped,
    no sticky bit), add or subtract as integers, normalize, truncate to 24 bits."""
    da, db = decode(a), decode(b)
    if da is None and db is None:
        return (a & b) & SIGN
    if da is None:
        return b
    if db is None:
        return a
    (sa, ea, ma), (sb, eb, mb) = da, db
    if (eb, mb) > (ea, ma):
        (sa, ea, ma), (sb, eb, mb) = (sb, eb, mb), (sa, ea, ma)
    d = ea - eb
    ma <<= guard_bits
    mb = (mb << guard_bits) >> d if d < 64 else 0
    m = ma + mb if sa == sb else ma - mb
    if m == 0:
        return 0
    e = ea
    width = 24 + guard_bits
    while m >= (1 << width):
        m >>= 1      # drop: truncation toward zero of the magnitude
        e += 1
    while m < (1 << (width - 1)):
        m <<= 1
        e -= 1
    m >>= guard_bits
    if e >= 256:
        return (sa << 31) | MAXB
    if e <= 0:
        return sa << 31
    return (sa << 31) | (e << 23) | (m & 0x7FFFFF)
