#!/usr/bin/env python3
"""Compare ADD/SUB candidate models (ee_fpu_models.py) with the real-PS2 rows of ps2autotests cpu/ee_fpu.

usage: fit_ee_add.py <ps2autotests>/tests/cpu/ee_fpu
Prints, per model, how many rows it reproduces, and the rows on which the models disagree with each other
(the discriminating rows). Category A evidence (known hardware vectors) only.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ee_fpu_models as m  # noqa: E402

CF = {'CF_ZERO': 0x00000000, 'CF_NEGZERO': 0x80000000, 'CF_MAX': 0x7FFFFFFF, 'CF_MIN': 0xFFFFFFFF,
      'CF_MAX_MANTISSA': 0x3FFFFFFF, 'CF_MAX_EXP': 0x7F800001, 'CF_MIN_EXP': 0x00000001, 'CF_NEGONE': 0xBF800000,
      'CF_ONE': 0x3F800000, 'CF_GARBAGE1': 0x00001337, 'CF_GARBAGE2': 0xDEADBEEF}
ROW = re.compile(r'^\s+(add|sub|adda|suba) (\S+), (\S+): ([0-9a-f]{8})')


def operand(tok):
    return CF[tok] if tok in CF else int(tok.split('/')[0], 16)


def rows(directory):
    out = []
    for path in sorted(pathlib.Path(directory).glob('*.expected')):
        for line in path.read_text(encoding='utf-8').splitlines():
            r = ROW.match(line)
            if r:
                a, b = operand(r.group(2)), operand(r.group(3))
                if r.group(1).startswith('sub'):
                    b ^= m.SIGN
                out.append((path.name, line.strip(), a, b, int(r.group(4), 16)))
    return out


MODELS = {'M1 exact toward zero': m.add_exact_toward_zero}
for g in range(0, 4):
    MODELS[f'M2 aligned, {g} guard bits'] = (lambda g: lambda a, b: m.add_aligned(a, b, g))(g)

if __name__ == '__main__':
    data = rows(sys.argv[1])
    print(f'{len(data)} ADD/SUB/ADDA/SUBA rows')
    preds = {name: [f(a, b) for _, _, a, b, _ in data] for name, f in MODELS.items()}
    for name, p in preds.items():
        bad = [(row[1], hex(row[4]), hex(x)) for row, x in zip(data, p) if x != row[4]]
        print(f'{name}: {len(data) - len(bad)}/{len(data)}' + (f'  e.g. {bad[:2]}' if bad else ''))
    names = list(preds)
    disc = [i for i in range(len(data)) if len({preds[n][i] for n in names}) > 1]
    print(f'{len(disc)} rows on which the models disagree')
    for i in disc[:12]:
        print('  ', data[i][1], ' hw', hex(data[i][4]), ' '.join(f'{n[:2]}{n[-13:-11]}={preds[n][i]:08x}' for n in names))
