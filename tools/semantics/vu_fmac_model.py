#!/usr/bin/env python3
"""VU FMAC contract (docs/architecture/vu-ee-arithmetic-semantics.md, section 3) in exact rational arithmetic.

A third implementation, independent of ps2_vu_fmac.h's integer reference and double-based lowering: it states the
rules on exact rationals and rounds once. Evidence class of its outputs: MODEL_PREDICTED.
"""
from fractions import Fraction

SIGN = 0x80000000
MAXB = 0x7FFFFFFF


def value(bits):
    e = (bits >> 23) & 0xFF
    if e == 0:
        return Fraction(0)
    v = Fraction((bits & 0x7FFFFF) | 0x800000) * Fraction(2) ** (e - 150)
    return -v if bits & SIGN else v


def encode_truncated(v, sign_if_zero):
    if v == 0:
        return sign_if_zero
    sign = SIGN if v < 0 else 0
    v = abs(v)
    e = 0
    while Fraction(2) ** (e + 1) <= v:
        e += 1
    while Fraction(2) ** e > v:
        e -= 1
    be = e + 127
    if be >= 256:          # >= 2^129
        return sign | MAXB
    if be <= 0:            # below 2^-126
        return sign
    m = int(v / Fraction(2) ** (e - 23))
    return sign | (be << 23) | (m & 0x7FFFFF)


def add(a, b):
    ea, eb = (a >> 23) & 0xFF, (b >> 23) & 0xFF
    if ea and eb and abs(ea - eb) >= 25:
        return a if ea > eb else b                       # A1
    if not ea and not eb:
        return a & b & SIGN
    s = value(a) + value(b)                              # A2
    return encode_truncated(s, 0)


def sub(a, b):
    return add(a, b ^ SIGN)


def mul(a, b):
    sign = (a ^ b) & SIGN
    if not (a >> 23) & 0xFF or not (b >> 23) & 0xFF:
        return sign
    return encode_truncated(value(a) * value(b), sign)  # M1


def nearest(v):
    """Round to nearest even (the pre-Phase-5 macro path), for choosing discriminating operands."""
    if v == 0:
        return 0
    sign = SIGN if v < 0 else 0
    v = abs(v)
    e = 0
    while Fraction(2) ** (e + 1) <= v:
        e += 1
    while Fraction(2) ** e > v:
        e -= 1
    scaled = v / Fraction(2) ** (e - 23)
    m = int(scaled)
    rem = scaled - m
    if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and m & 1):
        m += 1
        if m == 1 << 24:
            m, e = m >> 1, e + 1
    return sign | ((e + 127) << 23) | (m & 0x7FFFFF)
