#!/usr/bin/env python3
"""Z3 search for operands on which two EE ADD/SUB candidate models disagree (evidence register 1.2).

For each exponent gap d (concrete) the two mantissas are 24-bit bit-vectors (hidden bit set); each model's datapath is
encoded exactly (alignment, guard bits, subtraction of an opposite-sign operand, leading-one normalisation,
truncation). Z3 answers per (d, model pair):
  sat    -> a concrete counterexample, minimised and then re-checked against ee_fpu_models.py (independent code);
  unsat  -> the two models agree on every opposite-sign pair with that gap (a statement about the models only);
  unknown/timeout -> nothing is concluded.
Requires the optional z3-solver package; without it, use make_probes.py's deterministic search.

usage: z3_add_models.py [--gaps 0-40] [--json out.json]
"""
import argparse
import hashlib
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ee_fpu_models as em  # noqa: E402

try:
    import z3
except ImportError:  # pragma: no cover
    sys.exit('z3-solver is not installed (pip install z3-solver); use make_probes.py instead')

W = 128          # bit width used for all intermediate values (enough for gaps up to ~90 plus guard bits)
EA = 150         # exponent of the larger operand (any value keeping results in range works)


def normalise(value, scale_exponent):
    """value: W-bit unsigned, interpreted as value * 2^(scale_exponent - 150); -> (biased exponent, 23-bit fraction)."""
    exponent = z3.BitVecVal(0, 16)
    fraction = z3.BitVecVal(0, 32)
    for k in range(W - 1, -1, -1):           # highest set bit wins: build the If chain from the bottom up
        top = z3.Extract(k, k, value) == 1
        if k >= 23:
            frac_k = z3.Extract(31, 0, z3.LShR(value, k - 23)) & 0x7FFFFF
        else:
            frac_k = z3.Extract(31, 0, value << (23 - k)) & 0x7FFFFF
        exponent = z3.If(z3.And(top, z3.Extract(W - 1, k, value) == 1) if k == W - 1 else
                         z3.And(top, z3.Extract(W - 1, k + 1, value) == 0), z3.BitVecVal(scale_exponent + k - 23, 16), exponent)
        fraction = z3.If(z3.And(top, z3.Extract(W - 1, k + 1, value) == 0) if k < W - 1 else top, frac_k, fraction)
    return exponent, fraction


def model_result(name, ma, mb, d):
    """Magnitude of a - b for |a| > |b| (opposite signs), a = ma*2^(EA-150), b = mb*2^(EA-d-150)."""
    A = z3.ZeroExt(W - 24, ma)
    B = z3.ZeroExt(W - 24, mb)
    if name == 'M1':                       # exact difference, truncated
        S = (A << d) - B
        return normalise(S, EA - d)
    g = int(name.split('=')[1])            # M2 g=N: align keeping g guard bits, drop the rest
    S = (A << g) - z3.LShR(B << g, d)
    return normalise(S, EA - g)


def solve(m1, m2, d, timeout_ms):
    ma, mb = z3.BitVec('ma', 24), z3.BitVec('mb', 24)
    s = z3.Solver()
    s.set('timeout', timeout_ms)
    s.add(z3.Extract(23, 23, ma) == 1, z3.Extract(23, 23, mb) == 1)
    if d == 0:
        s.add(z3.UGT(ma, mb))
    e1, f1 = model_result(m1, ma, mb, d)
    e2, f2 = model_result(m2, ma, mb, d)
    s.add(z3.Or(e1 != e2, f1 != f2))
    r = s.check()
    if r != z3.sat:
        return str(r), None
    m = s.model()
    return 'sat', (m[ma].as_long(), m[mb].as_long())


def to_bits(ma, mb, d):
    a = (EA << 23) | (ma & 0x7FFFFF)
    b = 0x80000000 | ((EA - d) << 23) | (mb & 0x7FFFFF)
    return a, b


def python_model(name):
    if name == 'M1':
        return em.add_exact_toward_zero
    g = int(name.split('=')[1])
    return lambda a, b: em.add_aligned(a, b, g)


def minimise(a, b, f1, f2):
    """Clear mantissa bits while the two (independent python) models still disagree."""
    for which in (0, 1):
        for bit in range(22, -1, -1):
            trial = (a, b)
            x = trial[which] & ~(1 << bit)
            trial = (x, b) if which == 0 else (a, x)
            if f1(*trial) != f2(*trial):
                a, b = trial
    return a, b


def provenance(argv):
    """What produced a report: arguments, solver version, SHA-256 of the model sources (LF-normalised)."""
    here = pathlib.Path(__file__).resolve().parent
    sources = {}
    for name in ('z3_add_models.py', 'ee_fpu_models.py'):
        data = (here / name).read_bytes().replace(b'\r\n', b'\n')
        sources[f'tools/semantics/{name}'] = hashlib.sha256(data).hexdigest()
    return {'tool': 'tools/semantics/z3_add_models.py', 'args': argv, 'z3': z3.get_version_string(),
            'sources_sha256': sources, 'evidence_class': 'MODEL_PREDICTED',
            'note': 'sat = the two candidate models differ; unsat = they agree for that gap. Not hardware facts.'}


def write_report(path, report, argv):
    """Compact JSON: a provenance header, then one result per line (diffable, small)."""
    lines = ['{"provenance": ' + json.dumps(provenance(argv), sort_keys=True) + ',', ' "results": [']
    lines += ['  ' + json.dumps(e, sort_keys=True) + (',' if i + 1 < len(report) else '') for i, e in enumerate(report)]
    lines += [' ]}', '']
    path.write_text('\n'.join(lines), encoding='utf-8')


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--gaps', default='0-32')
    ap.add_argument('--models', default='M1,M2 g=0,M2 g=1,M2 g=2,M2 g=3')
    ap.add_argument('--timeout', type=int, default=20000)
    ap.add_argument('--json')
    args = ap.parse_args()
    lo, hi = (int(x) for x in args.gaps.split('-'))
    models = [m.strip() for m in args.models.split(',')]
    pairs = [(models[i], models[j]) for i in range(len(models)) for j in range(i + 1, len(models))]
    report = []
    for d in range(lo, hi + 1):
        row = []
        for m1, m2 in pairs:
            status, cex = solve(m1, m2, d, args.timeout)
            entry = {'gap': d, 'models': [m1, m2], 'z3': status}
            if cex:
                a, b = to_bits(*cex, d)
                f1, f2 = python_model(m1), python_model(m2)
                agree_independently = f1(a, b) != f2(a, b)
                a, b = minimise(a, b, f1, f2) if agree_independently else (a, b)
                entry.update(fs=f'{a:08x}', ft=f'{b:08x}', predictions={m1: f'{f1(a, b):08x}', m2: f'{f2(a, b):08x}'},
                             independently_confirmed=agree_independently)
            report.append(entry)
            row.append(('D' if cex and entry['independently_confirmed'] else '!' if cex else
                        '=' if status == 'unsat' else '?'))
        print(f'gap {d:2d}: ' + ' '.join(f'{p[0][-3:]}/{p[1][-3:]}:{c}' for p, c in zip(pairs, row)))
    print("legend: D = counterexample found and confirmed by the python models, = = proven equal for this gap (models only), "
          "! = solver counterexample NOT confirmed (encoding error), ? = unknown/timeout")
    if args.json:
        write_report(pathlib.Path(args.json), report, sys.argv[1:])
