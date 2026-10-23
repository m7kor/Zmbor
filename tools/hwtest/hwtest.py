#!/usr/bin/env python3
"""Hardware result tests: recompile test ELFs, run them natively and compare their output with output captured
on a real PS2 (for example unknownbrackets/ps2autotests, whose .expected files were printed by hardware).

  prepare --tests <dir> --recompiler <ps2_recomp> --out <dir> [--filter <substring>]...
      For every <name>.elf with a sibling <name>.expected: write a recompiler config, recompile, and write
      <out>/<id>/hwtest.cmake for ps2xTest/e2e (configure with -DPS2X_HWTEST_GENERATED=<out>).
      A failed recompile is recorded as RECOMPILE_FAIL in <out>/<id>/result.json.
  run --runner <exe> --elf <elf> --expected <file> --result <json> [--timeout <s>]
      Run one native build, compare the guest's text between "-- TEST BEGIN" and "-- TEST END" with the expected
      file, write the result JSON; exit status is 0 only for PASS.
  report --out <dir> [--json <file>] [--md <file>]
      Aggregate every result.json under <out> into a dashboard.

The .expected files are reference data; this tool never rewrites them.
"""
import argparse
import difflib
import json
import os
import pathlib
import subprocess
import sys
import time

BEGIN = '-- TEST BEGIN'
END = '-- TEST END'
STATUSES = ('PASS', 'MISMATCH', 'CRASH', 'TIMEOUT', 'NO_OUTPUT', 'NOT_BUILT', 'RECOMPILE_FAIL')


def test_id(tests_root, elf):
    rel = elf.relative_to(tests_root).with_suffix('')
    return '_'.join(rel.parts).replace('-', '_')


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def cmake_path(p):
    return str(p).replace('\\', '/')


def prepare(args):
    tests_root = pathlib.Path(args.tests).resolve()
    out = pathlib.Path(args.out).resolve()
    elfs = sorted(p for p in tests_root.rglob('*.elf') if p.with_suffix('.expected').exists())
    if args.filter:
        elfs = [p for p in elfs if any(f in cmake_path(p.relative_to(tests_root)) for f in args.filter)]
    for elf in elfs:
        tid = test_id(tests_root, elf)
        work = out / tid
        gen = work / 'generated'
        work.mkdir(parents=True, exist_ok=True)
        config = work / f'{tid}.toml'
        config.write_text(
            '[general]\n'
            f'input = "{cmake_path(elf)}"\n'
            f'output = "{cmake_path(gen)}"\n'
            'single_file_output = true\n'
            'stubs = []\n'
            'skip = []\n', encoding='utf-8')
        start = time.perf_counter()
        proc = subprocess.run([args.recompiler, str(config)], capture_output=True, text=True, errors='replace')
        elapsed = time.perf_counter() - start
        (work / 'recompile.log').write_text(proc.stdout + proc.stderr, encoding='utf-8')
        manifest = work / 'hwtest.cmake'
        base = {'id': tid, 'elf': cmake_path(elf.relative_to(tests_root)), 'recompile_seconds': round(elapsed, 3),
                'subsystem': subsystem_of(elf.relative_to(tests_root))}
        if proc.returncode != 0 or not any(gen.glob('*.cpp')):
            manifest.unlink(missing_ok=True)
            write_json(work / 'result.json', dict(base, status='RECOMPILE_FAIL',
                                                  detail=(proc.stderr or proc.stdout)[-2000:]))
            print(f'{tid}: RECOMPILE_FAIL')
            continue
        write_json(work / 'prepare.json', base)
        (work / 'result.json').unlink(missing_ok=True)
        manifest.write_text(
            f'set(HW_NAME "{tid}")\n'
            f'set(HW_ELF "{cmake_path(elf)}")\n'
            f'set(HW_EXPECTED "{cmake_path(elf.with_suffix(".expected"))}")\n'
            f'set(HW_CONFIG "{cmake_path(config)}")\n'
            f'set(HW_GEN "{cmake_path(gen)}")\n'
            f'set(HW_RESULT "{cmake_path(work / "result.json")}")\n', encoding='utf-8')
        print(f'{tid}: recompiled in {elapsed:.2f}s ({sum(1 for _ in gen.glob("*.cpp"))} sources)')
    return 0


def subsystem_of(rel):
    parts = pathlib.PurePath(rel).parts
    # tests/<area>/<unit>/<name>.elf, for example cpu/ee/alu or cpu/vu/... or kernel/thread/...
    return '/'.join(parts[:2]) if len(parts) > 2 else parts[0]


def normalize(text):
    lines = text.replace('\r\n', '\n').replace('\r', '\n').split('\n')
    return [line.rstrip() for line in lines]


def extract(stdout):
    """Guest text from the BEGIN marker to the END marker (inclusive), or None when BEGIN never appears."""
    lines = normalize(stdout)
    try:
        first = next(i for i, line in enumerate(lines) if line.endswith(BEGIN))
    except StopIteration:
        return None, False
    lines[first] = BEGIN  # the runtime may prefix guest lines; the marker itself is fixed text
    for i in range(first, len(lines)):
        if lines[i].endswith(END):
            lines[i] = END
            return lines[first:i + 1], True
    return lines[first:], False


def run(args):
    expected = normalize(pathlib.Path(args.expected).read_text(encoding='utf-8', errors='replace'))
    while expected and expected[-1] == '':
        expected.pop()
    result_path = pathlib.Path(args.result)
    tty_path = result_path.parent / 'run.tty.txt'
    tty_path.unlink(missing_ok=True)
    # The guest's tty (FILEIO fd 1/2) also goes to stdout, interleaved with runtime logging; compare the clean copy.
    env = dict(os.environ, PS2X_HIDDEN_WINDOW='1', PS2X_TTY_LOG=str(tty_path))
    prior = {}
    prepare_json = result_path.parent / 'prepare.json'
    if prepare_json.exists():
        prior = json.loads(prepare_json.read_text(encoding='utf-8'))
    start = time.perf_counter()
    timed_out = False
    try:
        proc = subprocess.run([args.runner, args.elf], capture_output=True, env=env, timeout=args.timeout)
        stdout, stderr, rc = proc.stdout, proc.stderr, proc.returncode
    except subprocess.TimeoutExpired as exc:
        stdout, stderr, rc, timed_out = exc.stdout or b'', exc.stderr or b'', None, True
    elapsed = time.perf_counter() - start
    stdout = stdout.decode('utf-8', errors='replace')
    stderr = stderr.decode('utf-8', errors='replace')
    (result_path.parent / 'run.stdout.txt').write_text(stdout, encoding='utf-8')
    (result_path.parent / 'run.stderr.txt').write_text(stderr[-200000:], encoding='utf-8')

    tty = tty_path.read_text(encoding='utf-8', errors='replace') if tty_path.exists() else ''
    actual, complete = extract(tty)
    result = dict(prior, run_seconds=round(elapsed, 3), exit_code=rc, expected_lines=len(expected))
    if actual is not None:
        while actual and actual[-1] == '':
            actual.pop()
        matcher = difflib.SequenceMatcher(a=expected, b=actual, autojunk=False)
        equal = sum(block.size for block in matcher.get_matching_blocks())
        result['matching_lines'] = equal
        result['actual_lines'] = len(actual)
        diff = list(difflib.unified_diff(expected, actual, 'expected', 'actual', n=1, lineterm=''))
        result['diff_head'] = diff[:80]
    if actual == expected and complete:
        status = 'PASS'
    elif timed_out:
        status = 'TIMEOUT'
    elif actual is None:
        status = 'NO_OUTPUT' if rc == 0 else 'CRASH'
    elif not complete and rc != 0:
        status = 'CRASH'
    else:
        status = 'MISMATCH'
    result['status'] = status
    if status != 'PASS':
        result['stderr_tail'] = stderr[-1500:]
    write_json(result_path, result)
    print(f'{status}: {pathlib.Path(args.elf).name} ({result.get("matching_lines", 0)}/{len(expected)} lines match,'
          f' exit {rc}, {elapsed:.2f}s)')
    if status != 'PASS':
        print('\n'.join(result.get('diff_head', [])[:40]))
        print(stderr[-1500:], file=sys.stderr)
    return 0 if status == 'PASS' else 1


def report(args):
    out = pathlib.Path(args.out).resolve()
    rows = []
    for work in sorted(p for p in out.iterdir() if p.is_dir()):
        result = work / 'result.json'
        if result.exists():
            rows.append(json.loads(result.read_text(encoding='utf-8')))
        elif (work / 'prepare.json').exists():
            rows.append(dict(json.loads((work / 'prepare.json').read_text(encoding='utf-8')), status='NOT_BUILT'))
    counts = {s: sum(1 for r in rows if r['status'] == s) for s in STATUSES}
    executed = sum(counts[s] for s in ('PASS', 'MISMATCH', 'CRASH', 'TIMEOUT', 'NO_OUTPUT'))
    summary = {
        'discovered': len(rows),
        'attempted': len(rows),
        'recompiled': len(rows) - counts['RECOMPILE_FAIL'],
        'built': len(rows) - counts['RECOMPILE_FAIL'] - counts['NOT_BUILT'],
        'executed': executed,
        'matching': counts['PASS'],
        'mismatch': counts['MISMATCH'],
        'crash': counts['CRASH'],
        'timeout': counts['TIMEOUT'],
        'no_output': counts['NO_OUTPUT'],
        'not_built': counts['NOT_BUILT'],
        'recompile_fail': counts['RECOMPILE_FAIL'],
        'expected_lines': sum(r.get('expected_lines', 0) for r in rows),
        'matching_lines': sum(r.get('matching_lines', 0) for r in rows),
    }
    by_subsystem = {}
    for r in rows:
        bucket = by_subsystem.setdefault(r.get('subsystem', '?'), {s: 0 for s in STATUSES})
        bucket[r['status']] += 1
    baseline = {}
    if args.baseline and pathlib.Path(args.baseline).exists():
        baseline = {t['id']: t['status'] for t in json.loads(pathlib.Path(args.baseline).read_text())['tests']}
    for r in rows:
        before = baseline.get(r['id'])
        r['regression'] = before == 'PASS' and r['status'] != 'PASS'
    dashboard = {'summary': summary, 'by_subsystem': by_subsystem,
                 'regressions': [r['id'] for r in rows if r['regression']],
                 'tests': [{k: r.get(k) for k in ('id', 'elf', 'subsystem', 'status', 'matching_lines',
                                                  'expected_lines', 'exit_code', 'run_seconds', 'regression')}
                           for r in rows]}
    if args.json:
        write_json(pathlib.Path(args.json), dashboard)
    md = ['# Hardware result dashboard', '',
          'Generated by `tools/hwtest/hwtest.py report`. Reference output: real-PS2 `.expected` files.', '',
          '| metric | count |', '|---|---|']
    md += [f'| {k} | {v} |' for k, v in summary.items()]
    md += ['', '| subsystem | ' + ' | '.join(STATUSES) + ' |', '|---|' + '---|' * len(STATUSES)]
    md += [f'| {s} | ' + ' | '.join(str(b[x]) for x in STATUSES) + ' |' for s, b in sorted(by_subsystem.items())]
    md += ['', '| test | status | lines matching | exit |', '|---|---|---|---|']
    md += [f'| {r["id"]} | {r["status"]}{" (REGRESSION)" if r["regression"] else ""} | '
           f'{r.get("matching_lines", "-")}/{r.get("expected_lines", "-")} | {r.get("exit_code", "-")} |' for r in rows]
    if args.md:
        pathlib.Path(args.md).write_text('\n'.join(md) + '\n', encoding='utf-8')
    print('\n'.join(md))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('--tests', required=True)
    p.add_argument('--recompiler', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--filter', action='append')
    p = sub.add_parser('run')
    p.add_argument('--runner', required=True)
    p.add_argument('--elf', required=True)
    p.add_argument('--expected', required=True)
    p.add_argument('--result', required=True)
    p.add_argument('--timeout', type=float, default=60)
    p = sub.add_parser('report')
    p.add_argument('--out', required=True)
    p.add_argument('--json')
    p.add_argument('--md')
    p.add_argument('--baseline', help='earlier dashboard JSON; tests that passed there and fail now are regressions')
    args = parser.parse_args()
    return {'prepare': prepare, 'run': run, 'report': report}[args.command](args)


if __name__ == '__main__':
    sys.exit(main())
