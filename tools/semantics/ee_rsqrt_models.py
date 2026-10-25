#!/usr/bin/env python3
"""EE RSQRT.S: does it compose the SQRT.S and DIV.S datapaths? (evidence register 1.4/1.5)

Train/validation split, no fitting on the rows being explained:
  1. SQRT.S hardware rows choose the root rounding R  (train set 1: cpu/ee_fpu/sqrt.expected, section sqrt)
  2. DIV.S hardware rows choose the quotient rounding Q (train set 2: cpu/ee_fpu/muldiv.expected, section div)
  3. the composition RSQRT(a, b) = Q(a / R(sqrt|b|)) is then a prediction for the RSQRT.S rows (validation).
Every (R, Q) combination is also scored on the validation rows and reported, so a better-fitting combination that
the train sets did not pick is visible rather than hidden.

All arithmetic is exact: square roots and quotients are compared through squares/cross-multiplication, so no rounding
decision depends on a host float. Operands follow the PS2 format (exponent 0 = zero, exponent 255 = value).

usage: ee_rsqrt_models.py [--autotests <ps2autotests root>]
"""
import argparse
import pathlib
import re
from fractions import Fraction
from math import isqrt

CF = {'CF_ZERO': 0x00000000, 'CF_NEGZERO': 0x80000000, 'CF_MAX': 0x7FFFFFFF, 'CF_MIN': 0xFFFFFFFF,
      'CF_MAX_MANTISSA': 0x3FFFFFFF, 'CF_MAX_EXP': 0x7F800001, 'CF_MIN_EXP': 0x00000001, 'CF_NEGONE': 0xBF800000,
      'CF_ONE': 0x3F800000, 'CF_GARBAGE1': 0x00001337, 'CF_GARBAGE2': 0xDEADBEEF}
MAXB = 0x7FFFFFFF


def value(bits):
    e = (bits >> 23) & 0xFF
    if e == 0:
        return Fraction(0)
    v = Fraction((bits & 0x7FFFFF) | 0x800000) * Fraction(2) ** (e - 150)
    return -v if bits >> 31 else v


def pack(sign, e, m):
    """m in [2^23, 2^24) at unbiased exponent e; saturate/flush in the PS2 format."""
    if m == 1 << 24:
        m, e = m >> 1, e + 1
    be = e + 127
    if be >= 256:
        return sign | MAXB
    if be <= 0:
        return sign
    return sign | (be << 23) | (m & 0x7FFFFF)


def round_value(sign, below, mode):
    """below(t) -> True when the exact (positive) value is < t. Returns PS2 bits rounded by mode."""
    # Exponent: largest e with 2^e <= v.
    lo, hi = -300, 300
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if below(Fraction(2) ** mid):
            hi = mid
        else:
            lo = mid
    e = lo
    ulp = Fraction(2) ** (e - 23)
    lo, hi = 1 << 23, 1 << 24          # mantissa m with m*ulp <= v < (m+1)*ulp
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if below(mid * ulp):
            hi = mid
        else:
            lo = mid
    m = lo
    if mode == 'trunc':
        return pack(sign, e, m)
    if mode == 'near':
        half = (2 * m + 1) * ulp / 2
        exact_half = not below(half) and below(half + Fraction(1, 1 << 400))
        if not below(half) and not (exact_half and m % 2 == 0):
            m += 1
        return pack(sign, e, m)
    if mode == 'up':                    # away from zero (any non-zero remainder)
        if below(m * ulp + Fraction(1, 1 << 400)) and not below(m * ulp):
            return pack(sign, e, m)     # exact
        return pack(sign, e, m + 1)
    raise ValueError(mode)


def sqrt_bits(b, mode):
    v = abs(value(b))
    if v == 0:
        return b & 0x80000000 if mode != 'ignore-sign' else 0
    return round_value(0, lambda t: v < t * t, mode)


def div_bits(a, b, mode):
    """DIV.S datapath for non-zero b; |a|/|b| rounded by mode."""
    va, vb = value(a), value(b)
    sign = (a ^ b) & 0x80000000
    if vb == 0:
        return sign | MAXB
    if va == 0:
        return sign
    q = abs(va) / abs(vb)
    return round_value(sign, lambda t: q < t, mode)


def rsqrt_composed(a, b, root_mode, div_mode):
    if value(b) == 0:
        return (a & 0x80000000) | MAXB
    if value(a) == 0:
        return a & 0x80000000
    sign = a & 0x80000000
    if root_mode == 'exact':            # a / sqrt(|b|) without an intermediate rounding
        va2, vb = value(a) ** 2, abs(value(b))
        return round_value(sign, lambda t: va2 < t * t * vb, div_mode)
    root = sqrt_bits(b & 0x7FFFFFFF, root_mode)
    return div_bits(a & 0x7FFFFFFF | sign, root, div_mode)


def mul_bits(a, b, mode):
    va, vb = value(a), value(b)
    sign = (a ^ b) & 0x80000000
    if va == 0 or vb == 0:
        return sign
    p = abs(va * vb)
    return round_value(sign, lambda t: p < t, mode)


def rsqrt_reciprocal(a, b, root_mode, recip_mode, mul_mode):
    """Alternative datapath: a * R(1 / R(sqrt|b|)), the reciprocal and the product each rounded."""
    if value(b) == 0:
        return (a & 0x80000000) | MAXB
    if value(a) == 0:
        return a & 0x80000000
    root = sqrt_bits(b & 0x7FFFFFFF, root_mode)
    return mul_bits(a, div_bits(0x3F800000, root, recip_mode), mul_mode)


def div_reciprocal(a, b, recip_mode, mul_mode):
    """The same reciprocal datapath applied to DIV.S (a cross-check: does DIV.S share it?)."""
    if value(b) == 0:
        return ((a ^ b) & 0x80000000) | MAXB
    if value(a) == 0:
        return (a ^ b) & 0x80000000
    return mul_bits(a, div_bits(0x3F800000 | (b & 0x80000000), b, recip_mode), mul_mode)


def parse_tok(tok):
    tok = tok.strip()
    return CF[tok] if tok in CF else int(tok.split('/')[0], 16)


def rows(path, section, arity):
    out, current = [], None
    for line in path.read_text(encoding='utf-8').splitlines():
        if re.match(r'^\w+:$', line):
            current = line[:-1]
            continue
        if current != section or not line.strip().startswith(section + ' '):
            continue
        body, result = line.strip()[len(section) + 1:].rsplit(':', 1)
        ops = [parse_tok(t) for t in body.split(',')]
        if len(ops) == arity:
            out.append((tuple(ops), int(result.strip().split('/')[0], 16)))
    return list(dict.fromkeys(out))       # duplicate rows (CF_ONE twice etc.) count once


def score(rows_, f):
    bad = [(ops, want, f(*ops)) for ops, want in rows_ if f(*ops) != want]
    return len(rows_) - len(bad), bad


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    here = pathlib.Path(__file__).resolve()
    ap.add_argument('--autotests', default=str(here.parents[3] / 'thirdparty-data' / 'ps2autotests'))
    args = ap.parse_args()
    d = pathlib.Path(args.autotests) / 'tests' / 'cpu' / 'ee_fpu'
    sqrt_rows = rows(d / 'sqrt.expected', 'sqrt', 1)
    div_rows = rows(d / 'muldiv.expected', 'div', 2)
    rsqrt_rows = rows(d / 'sqrt.expected', 'rsqrt', 2)
    print(f'rows: sqrt {len(sqrt_rows)} (train), div {len(div_rows)} (train), rsqrt {len(rsqrt_rows)} (validation)')
    modes = ['trunc', 'near', 'up']
    best = {}
    for name, rs, fn in (('SQRT.S', sqrt_rows, lambda m: (lambda b: sqrt_bits(b, m))),
                         ('DIV.S', div_rows, lambda m: (lambda a, b: div_bits(a, b, m)))):
        scores = {m: score(rs, fn(m)) for m in modes}
        for m in modes:
            print(f'  {name:7s} rounding {m:5s}: {scores[m][0]}/{len(rs)}')
        top = max(scores[m][0] for m in modes)
        best[name] = [m for m in modes if scores[m][0] == top]
        if top != len(rs):
            for ops, want, got in scores[best[name][0]][1][:6]:
                print('     miss', ' '.join(f'{x:08x}' for x in ops), f'hw {want:08x} model {got:08x}')
    print(f"train-set choice: root {best['SQRT.S']}, quotient {best['DIV.S']}")
    print('validation (RSQRT.S rows), every combination; * = chosen by the train sets:')
    for rm in ['exact'] + modes:
        for qm in modes:
            n, bad = score(rsqrt_rows, lambda a, b: rsqrt_composed(a, b, rm, qm))
            mark = '*' if rm in best['SQRT.S'] and qm in best['DIV.S'] else ' '
            print(f' {mark} root {rm:5s} quotient {qm:5s}: {n}/{len(rsqrt_rows)}'
                  + ''.join(f"\n      miss {a:08x} {b:08x}: hw {w:08x} model {g:08x}" for (a, b), w, g in bad[:4]))
    print('alternative datapath a * R(1/R(sqrt b)) - chosen AFTER seeing the rows above (post hoc), so a fit here is')
    print('not a validated prediction; the same datapath for DIV.S is scored as a cross-check:')
    for rm in modes:
        for cm in modes:
            for pm in modes:
                n, bad = score(rsqrt_rows, lambda a, b: rsqrt_reciprocal(a, b, rm, cm, pm))
                if n >= len(rsqrt_rows) - 1:
                    nd, _ = score(div_rows, lambda a, b: div_reciprocal(a, b, cm, pm))
                    print(f'   root {rm:5s} reciprocal {cm:5s} product {pm:5s}: {n}/{len(rsqrt_rows)}; '
                          f'DIV.S by the same datapath {nd}/{len(div_rows)}')
