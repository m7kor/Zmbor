#!/usr/bin/env python3
"""Candidate models of EE COP1 MUL.S and of the VU FMAC multiplier, in exact integer arithmetic.

All fit the 72 MUL/MULA rows of ps2autotests cpu/ee_fpu (see fit_ee_mul.py); they differ elsewhere.
  hot      Booth radix-4 recoding of ft, the lowest digit's +1 missing when it is negative (production EE MUL.S)
  trunc K  truncated array: partial-product columns below K dropped, carries out of them lost
  exact    exact product truncated (production VU FMAC MUL; fails 6 of the EE rows)
"""

MASK48 = (1 << 48) - 1
SIGN = 0x80000000
MAXB = 0x7FFFFFFF


def booth_digits(b):
    digits, prev = [], 0
    for i in range(13):
        b0, b1 = (b >> (2 * i)) & 1, (b >> (2 * i + 1)) & 1
        digits.append(-2 * b1 + b0 + prev)
        prev = b1
    return digits


def product_truncated_array(a, b, k, width=52):
    mask = (1 << width) - 1
    total = 0
    for i, d in enumerate(booth_digits(b)):
        m = abs(d) * a
        term = ((~m) << (2 * i)) & mask if d < 0 else (m << (2 * i)) & mask
        total += term >> k
        if d < 0:
            total += (1 << (2 * i)) >> k
    return (total << k) & MASK48


def finish(sign, ea, eb, p):
    e = ea + eb - 127
    if p >> 47:
        mant, e = p >> 24, e + 1
    else:
        mant = p >> 23
    if e >= 256:
        return sign | MAXB
    if e <= 0:
        return sign
    return sign | (e << 23) | (mant & 0x7FFFFF)


def mul(x, y, model):
    sign = (x ^ y) & SIGN
    ex, ey = (x >> 23) & 0xFF, (y >> 23) & 0xFF
    if ex == 0 or ey == 0:
        return sign
    a, b = (x & 0x7FFFFF) | 0x800000, (y & 0x7FFFFF) | 0x800000
    if model == 'hot':
        p = a * b - ((b >> 1) & 1)
    elif model == 'exact':
        p = a * b
    else:
        p = product_truncated_array(a, b, int(model))
    return finish(sign, ex, ey, p)


EE_MUL_MODELS = ['hot', '1', '2', '4', '8', '16', '22']
