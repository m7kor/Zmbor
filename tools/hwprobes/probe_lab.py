#!/usr/bin/env python3
"""Hardware-probe manifests, observation ingest and model scoring (docs/architecture/hardware-probes.md).

  probe_lab.py manifest [--dir generated]           (re)write <dir>/manifest.json from the *.predictions.csv files
  probe_lab.py ingest <probe> <observations.txt> --source {hardware,emulator,zmbor} --provenance "<text>"
                                                    score every model of <probe> against observed "<fs> <ft> <result>"
                                                    lines and record the observation in the manifest
  probe_lab.py selftest                             pipeline test on synthetic data (no hardware involved)

Evidence classes (never promoted by this tool):
  HARDWARE_CONFIRMED  observed on a physical PS2 by this project (source=hardware, with a console identifier)
  HARDWARE_REFERENCE  published real-PS2 vectors (ps2autotests .expected) - not produced here
  EMULATOR_OBSERVED   output of an emulator (PCSX2 etc.); never hardware evidence
  MODEL_PREDICTED     output of a model in tools/semantics; never hardware evidence
  ZMBOR_OBSERVED      Zmbor's own output; never hardware evidence
  UNMEASURED          no observation exists
"""
import argparse
import csv
import datetime
import hashlib
import json
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MODEL_SOURCES = ['tools/semantics/ee_fpu_models.py', 'tools/semantics/mul_models.py', 'tools/semantics/make_probes.py',
                 'tools/semantics/z3_add_models.py', 'tools/semantics/ee_rsqrt_models.py',
                 'tools/semantics/make_macro_flag_probe.py', 'tools/semantics/vu_fmac_model.py']

QUESTIONS = {
    'p1_ee_add_guard_bits': dict(register='1.2', unit='EE COP1', instruction='ADD.S',
                                 question='How many guard bits does the EE adder keep when aligning, and is there a '
                                          'sticky bit? (M1 = exact toward zero, M2 g = g guard bits, no sticky)'),
    'p2_ee_mul_partial_products': dict(register='1.3', unit='EE COP1', instruction='MUL.S',
                                       question='Which partial-product truncation reproduces the EE multiplier?'),
    'p3_ee_rsqrt_datapath': dict(register='1.5', unit='EE COP1', instruction='RSQRT.S',
                                 question='Is RSQRT.S a rounded root then a divide, or a rounded reciprocal of the '
                                          'rounded root times fs? (the latter fits 34/34 rows, chosen post hoc)'),
    'p4_vu_mul_asymmetry': dict(register='2.3', unit='VU0 macro', instruction='VMUL.x',
                                question='Is the VU multiplier operand-order asymmetric like the EE one?'),
    'p5_vu0_macro_flags': dict(register='2.4', unit='VU0 macro', instruction='VADD + CFC2 vi16/vi17',
                               question='Does a macro FMAC op update MAC/STATUS, immediately or after the flag latency?'),
}

SOURCE_CLASS = {'hardware': 'HARDWARE_CONFIRMED', 'emulator': 'EMULATOR_OBSERVED', 'zmbor': 'ZMBOR_OBSERVED',
                'synthetic': 'MODEL_PREDICTED'}


def read_predictions(path):
    rows = [line for line in path.read_text(encoding='utf-8').splitlines() if line and not line.startswith('#')]
    reader = csv.reader(rows)
    header = next(reader)
    models = header[2:]
    body = list(reader)
    if any(len(r) != len(header) for r in body):
        raise ValueError(f'{path.name}: rows and header disagree in width (a model name containing a comma?)')
    table = [(int(r[0], 16), int(r[1], 16), {m: int(v, 16) for m, v in zip(models, r[2:])}) for r in body]
    return models, table


def read_structured(path):
    """Structured probe: a fixed sequence of readings; model predictions may refer to the first observed readings
    ("m0", "s0") or set bits on them ("s0|0x1c7")."""
    data = json.loads(path.read_text(encoding='utf-8'))
    return list(data['models']), data['readings'], data['models']


def resolve(token, observed):
    if token.startswith('m0') or token.startswith('s0'):
        base = observed[0] if token.startswith('m0') else observed[1]
        return base | int(token.split('|', 1)[1], 16) if '|' in token else base
    return int(token, 16)


def sources_digest():
    h = hashlib.sha256()
    for rel in MODEL_SOURCES:
        p = ROOT / rel
        if p.exists():
            h.update(rel.encode() + b'\0' + p.read_bytes().replace(b'\r\n', b'\n'))
    return h.hexdigest()


def load_manifest(directory):
    p = directory / 'manifest.json'
    return json.loads(p.read_text(encoding='utf-8')) if p.exists() else {'probes': {}}


def save_manifest(directory, manifest):
    (directory / 'manifest.json').write_text(json.dumps(manifest, indent=1, sort_keys=True) + '\n', encoding='utf-8')


def cmd_manifest(directory):
    old = load_manifest(directory)
    manifest = {'schema': 1, 'generator': 'tools/semantics/make_probes.py', 'model_sources_sha256': sources_digest(),
                'note': 'Predictions are MODEL_PREDICTED. No probe here has been built or run on a PS2.', 'probes': {}}
    for csv_path in sorted(directory.glob('*.predictions.csv')):
        name = csv_path.name[:-len('.predictions.csv')]
        models, table = read_predictions(csv_path)
        observations = old.get('probes', {}).get(name, {}).get('observations', [])
        classes = {o['evidence_class'] for o in observations}
        manifest['probes'][name] = dict(
            QUESTIONS.get(name, {}), source=f'{name}.cpp', predictions=csv_path.name, operand_pairs=len(table),
            models=models, prediction_class='MODEL_PREDICTED',
            build='ps2sdk + ps2autotests (not available in this environment)',
            hardware_status='HARDWARE_CONFIRMED' if 'HARDWARE_CONFIRMED' in classes else 'UNMEASURED',
            observations=observations)
    for json_path in sorted(directory.glob('*.predictions.json')):
        name = json_path.name[:-len('.predictions.json')]
        models, readings, _ = read_structured(json_path)
        observations = old.get('probes', {}).get(name, {}).get('observations', [])
        classes = {o['evidence_class'] for o in observations}
        manifest['probes'][name] = dict(
            QUESTIONS.get(name, {}), source=f'{name}.cpp', predictions=json_path.name, structured=True,
            operand_pairs=len(readings), models=models, prediction_class='MODEL_PREDICTED',
            build='ps2sdk + ps2autotests (not available in this environment)',
            hardware_status='HARDWARE_CONFIRMED' if 'HARDWARE_CONFIRMED' in classes else 'UNMEASURED',
            observations=observations)
    save_manifest(directory, manifest)
    for name, p in manifest['probes'].items():
        print(f"{name}: {p['operand_pairs']} {'readings' if p.get('structured') else 'pairs'}, {len(p['models'])} models, "
              f"hardware {p['hardware_status']}")
    return manifest


def parse_observations(path):
    out = {}
    for line in pathlib.Path(path).read_text(encoding='utf-8').splitlines():
        parts = line.split()
        if len(parts) == 3 and all(len(x) == 8 for x in parts):
            try:
                fs, ft, r = (int(x, 16) for x in parts)
            except ValueError:
                continue
            out[(fs, ft)] = r
    return out


def score(models, table, observed):
    result = {m: {'match': 0, 'mismatch': []} for m in models}
    missing = 0
    for fs, ft, preds in table:
        if (fs, ft) not in observed:
            missing += 1
            continue
        for m in models:
            if preds[m] == observed[(fs, ft)]:
                result[m]['match'] += 1
            else:
                result[m]['mismatch'].append(f'{fs:08x} {ft:08x}: predicted {preds[m]:08x}, observed {observed[(fs, ft)]:08x}')
    return result, missing


def cmd_ingest(directory, probe, obs_path, source, provenance, console=None, record=True):
    if source == 'hardware' and not console:
        sys.exit('ingest: --source hardware requires --console (a physical console identifier); '
                 'an emulator or Zmbor run is not hardware')
    structured = directory / f'{probe}.predictions.json'
    if structured.exists():
        models, readings, predictions = read_structured(structured)
        values = [int(t, 16) for t in pathlib.Path(obs_path).read_text(encoding='utf-8').split()
                  if len(t) == 8 and all(c in '0123456789abcdefABCDEF' for c in t)]
        missing = max(0, len(readings) - len(values))
        table = readings
        result = {m: {'match': 0, 'mismatch': []} for m in models}
        if not missing:
            for m in models:
                for i, name in enumerate(readings):
                    want = resolve(predictions[m][i], values)
                    if want == values[i]:
                        result[m]['match'] += 1
                    else:
                        result[m]['mismatch'].append(f'{name}: predicted {want:08x}, observed {values[i]:08x}')
    else:
        models, table = read_predictions(directory / f'{probe}.predictions.csv')
        observed = parse_observations(obs_path)
        result, missing = score(models, table, observed)
    evidence = SOURCE_CLASS[source]
    complete = missing == 0
    surviving = [m for m in models if not result[m]['mismatch']] if complete else []
    print(f'{probe}: {len(table) - missing}/{len(table)} pairs observed, evidence class {evidence}')
    for m in models:
        print(f"  {m:24s} {result[m]['match']}/{len(table) - missing}")
    if not complete:
        print('  incomplete observation: no model is selected')
    elif evidence != 'HARDWARE_CONFIRMED':
        print(f'  consistent models {surviving} - {evidence}, NOT hardware evidence')
    else:
        print(f'  consistent models on hardware: {surviving}')
    if record:
        manifest = load_manifest(directory)
        entry = manifest['probes'].setdefault(probe, {}).setdefault('observations', [])
        entry.append({'evidence_class': evidence, 'source': source, 'provenance': provenance, 'console': console,
                      'date': datetime.date.today().isoformat(), 'pairs_observed': len(table) - missing,
                      'consistent_models': surviving, 'scores': {m: result[m]['match'] for m in models}})
        if evidence == 'HARDWARE_CONFIRMED' and complete:
            manifest['probes'][probe]['hardware_status'] = 'HARDWARE_CONFIRMED'
        save_manifest(directory, manifest)
    return evidence, surviving, missing


def cmd_selftest():
    """Synthetic data only: checks that scoring selects the generating model and that classes are never promoted."""
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        src = HERE / 'generated' / 'p1_ee_add_guard_bits.predictions.csv'
        (d / src.name).write_text(src.read_text(encoding='utf-8'), encoding='utf-8')
        models, table = read_predictions(d / src.name)
        cmd_manifest(d)
        for truth in models:
            obs = d / 'obs.txt'
            obs.write_text(''.join(f'{fs:08x} {ft:08x} {p[truth]:08x}\n' for fs, ft, p in table), encoding='utf-8')
            evidence, surviving, _ = cmd_ingest(d, 'p1_ee_add_guard_bits', obs, 'synthetic', 'selftest', record=False)
            if surviving != [truth] or evidence != 'MODEL_PREDICTED':
                print(f'FAIL: generated from {truth}, analysis kept {surviving} ({evidence})')
                failures += 1
        # An emulator observation must stay EMULATOR_OBSERVED and leave the probe UNMEASURED.
        obs.write_text(''.join(f'{fs:08x} {ft:08x} {p[models[0]]:08x}\n' for fs, ft, p in table), encoding='utf-8')
        cmd_ingest(d, 'p1_ee_add_guard_bits', obs, 'emulator', 'selftest emulator')
        status = load_manifest(d)['probes']['p1_ee_add_guard_bits']['hardware_status']
        if status != 'UNMEASURED':
            print(f'FAIL: emulator observation changed hardware status to {status}')
            failures += 1
        # Missing rows: nothing is selected.
        obs.write_text(''.join(f'{fs:08x} {ft:08x} {p[models[0]]:08x}\n' for fs, ft, p in table[1:]), encoding='utf-8')
        _, surviving, missing = cmd_ingest(d, 'p1_ee_add_guard_bits', obs, 'synthetic', 'selftest partial', record=False)
        if surviving or missing != 1:
            print('FAIL: partial observation selected a model')
            failures += 1
    # Structured probe (P5): an observation generated from each model, with arbitrary initial MAC/STATUS, must be
    # consistent with that model only.
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        src = HERE / 'generated' / 'p5_vu0_macro_flags.predictions.json'
        (d / src.name).write_text(src.read_text(encoding='utf-8'), encoding='utf-8')
        models, readings, predictions = read_structured(d / src.name)
        for truth in models:
            initial = [0x00000010, 0x00000040]  # an arbitrary m0, s0 (sticky ZS already set)
            values = []
            for i, token in enumerate(predictions[truth]):
                values.append(initial[i] if i < 2 else resolve(token, initial))
            obs = d / 'obs.txt'
            obs.write_text(''.join(f'{v:08x}\n' for v in values), encoding='utf-8')
            evidence, surviving, _ = cmd_ingest(d, 'p5_vu0_macro_flags', obs, 'synthetic', 'selftest', record=False)
            if surviving != [truth]:
                print(f'FAIL: P5 observation generated from {truth} kept {surviving}')
                failures += 1
    csvs_and_json = {p.name.split('.predictions.')[0] for p in (HERE / 'generated').glob('*.predictions.*')}
    # The committed manifest: covers every probe, claims nothing it has not earned.
    committed = load_manifest(HERE / 'generated')
    if set(committed['probes']) != csvs_and_json:
        print(f'FAIL: manifest.json lists {sorted(committed["probes"])}, predictions exist for {sorted(csvs_and_json)}')
        failures += 1
    for name, p in committed['probes'].items():
        hw = [o for o in p.get('observations', []) if o['evidence_class'] == 'HARDWARE_CONFIRMED']
        if p.get('prediction_class') != 'MODEL_PREDICTED' or any(not o.get('console') for o in hw) or \
                (p.get('hardware_status') == 'HARDWARE_CONFIRMED') != bool(hw):
            print(f'FAIL: manifest entry {name} claims an evidence class it has not earned')
            failures += 1
    print('selftest:', 'PASS' if not failures else f'{failures} FAILURES')
    return failures


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('manifest')
    m.add_argument('--dir', default=str(HERE / 'generated'))
    i = sub.add_parser('ingest')
    i.add_argument('probe')
    i.add_argument('observations')
    i.add_argument('--source', required=True, choices=['hardware', 'emulator', 'zmbor'])
    i.add_argument('--provenance', required=True)
    i.add_argument('--console')
    i.add_argument('--dir', default=str(HERE / 'generated'))
    sub.add_parser('selftest')
    a = ap.parse_args()
    if a.cmd == 'manifest':
        cmd_manifest(pathlib.Path(a.dir))
    elif a.cmd == 'ingest':
        cmd_ingest(pathlib.Path(a.dir), a.probe, a.observations, a.source, a.provenance, a.console)
    else:
        sys.exit(1 if cmd_selftest() else 0)
