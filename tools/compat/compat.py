#!/usr/bin/env python3
"""Compatibility matrix for programs run through the Zmbor pipeline (docs/architecture/compatibility-matrix.md).

  compat.py collect --hwtest-dir <prepared dir> --build <build dir> --inputs <dir holding the ELFs> --label <set name>
                    --out <matrix.json> [--md <matrix.md>] [--repeat-runner]
  compat.py validate <matrix.json>

Every entry records only what an actual run observed. Stages (a program is credited with the highest stage whose
evidence exists; later stages are "untested" unless a run exercised them):
  0 input recognized      ELF parsed by the recompiler (prepare.json written)
  1 recompilation         ps2_recomp produced sources (no RECOMPILE_FAIL)
  2 native compilation    the host executable exists (no NOT_BUILT)
  3 runtime initialised   the program started and produced guest output
  4 stable checkpoint     a program-defined end marker was reached and the process exited 0
  5-9 interactive / gameplay / extended / persistence / broad: never credited by this collector (no input scripts,
      renderer checks or save tests are run); they stay "untested"
Correctness against a reference (here: real-PS2 .expected output) is a separate field, not a stage.
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile

SCHEMA_VERSION = 1
STAGE_NAMES = ['input_recognized', 'recompilation', 'native_compilation', 'runtime_initialised', 'stable_checkpoint',
               'interactive', 'main_workload', 'extended_execution', 'persistence', 'broad_compatibility']
FAILURE_CLASSES = ['none', 'recompiler_crash', 'code_generation_failure', 'host_compiler_failure',
                   'unsupported_instruction', 'unresolved_guest_call', 'guest_exception', 'memory_ownership_failure',
                   'scheduler_deadlock', 'iop_service_failure', 'dma_vif_gif_failure', 'graphics_mismatch',
                   'audio_failure', 'performance_stall', 'host_crash', 'output_mismatch', 'test_harness_failure',
                   'external_dependency_missing', 'unclassified']

# First matching rule wins; each names the runtime message it relies on.
STDERR_RULES = [
    ('unsupported_instruction', re.compile(r'Unhandled|unimplemented instruction|reserved (upper|lower)', re.I)),
    ('unresolved_guest_call', re.compile(r'No exact recompiled function for guest PC|unresolved (call|jump)', re.I)),
    ('memory_ownership_failure', re.compile(r'overlaps the runtime|out of range|invalid address|arena .* cannot fit', re.I)),
    ('iop_service_failure', re.compile(r'\[IOP:load-failed\]|no HLE provider|SIF .* failed', re.I)),
    ('dma_vif_gif_failure', re.compile(r'\[(DMA|VIF|GIF)[^\]]*\].*(error|invalid|fail)', re.I)),
    ('guest_exception', re.compile(r'guest exception|SignalException|address error|TLB', re.I)),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def git(*args):
    try:
        return subprocess.run(['git', *args], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def classify(result, stderr_text):
    status = result.get('status')
    if status == 'PASS':
        return 'none'
    if status == 'RECOMPILE_FAIL':
        return 'recompiler_crash' if 'Traceback' in stderr_text or 'exception' in stderr_text.lower() else 'code_generation_failure'
    if status == 'NOT_BUILT':
        return 'host_compiler_failure'
    if status == 'TIMEOUT':
        return 'performance_stall' if result.get('actual_lines') else 'scheduler_deadlock'
    for name, rule in STDERR_RULES:
        if rule.search(stderr_text):
            return name
    if status == 'CRASH':
        return 'host_crash'
    if status == 'NO_OUTPUT':
        return 'unclassified'
    if status == 'MISMATCH':
        return 'output_mismatch'
    return 'unclassified'


def stage_of(result, has_prepare, has_binary):
    status = result.get('status') if result else None
    if not has_prepare:
        return -1
    if status == 'RECOMPILE_FAIL':
        return 0
    if not has_binary or status == 'NOT_BUILT':
        return 1
    if status in (None, 'NO_OUTPUT') or not result.get('actual_lines'):
        return 2
    if status in ('PASS', 'MISMATCH') and result.get('exit_code') == 0:
        return 4
    return 3


def first_divergence(result):
    for line in result.get('diff_head', [])[2:]:
        if line.startswith('-') or line.startswith('+'):
            return line[:200]
    return None


def repeat_tty(runner, elf):
    """Runs the program a second time and returns its guest output, for a determinism check."""
    with tempfile.TemporaryDirectory() as tmp:
        tty = pathlib.Path(tmp) / 'tty.txt'
        env = dict(__import__('os').environ, PS2X_HIDDEN_WINDOW='1', PS2X_TTY_LOG=str(tty))
        try:
            subprocess.run([str(runner), str(elf)], capture_output=True, env=env, timeout=120)
        except subprocess.TimeoutExpired:
            return None
        return tty.read_text(encoding='utf-8', errors='replace') if tty.exists() else None


def collect(args):
    hw = pathlib.Path(args.hwtest_dir)
    build = pathlib.Path(args.build)
    inputs = pathlib.Path(args.inputs)
    recompiler = build / 'ps2xRecomp' / ('ps2_recomp.exe' if (build / 'ps2xRecomp' / 'ps2_recomp.exe').exists() else 'ps2_recomp')
    runtime = next((p for p in (build / 'ps2xRuntime').glob('*ps2_runtime*') if p.suffix in ('.lib', '.a')), None)
    matrix = {
        'schema': SCHEMA_VERSION,
        'set': args.label,
        'generated': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
        'source': {'commit': git('rev-parse', 'HEAD'), 'tree_clean': git('status', '--porcelain') == ''},
        'tools': {'recompiler_sha256': sha256(recompiler) if recompiler.exists() else None,
                  'runtime_sha256': sha256(runtime) if runtime else None},
        'stage_names': STAGE_NAMES,
        'programs': [],
    }
    for prepare in sorted(hw.glob('*/prepare.json')):
        d = prepare.parent
        meta = json.loads(prepare.read_text(encoding='utf-8'))
        result_path = d / 'result.json'
        result = json.loads(result_path.read_text(encoding='utf-8')) if result_path.exists() else {}
        exe = next(iter((build / 'ps2xTest' / 'e2e').glob(f"ps2x_hw_{meta['id']}*")), None)
        stderr_file = d / 'run.stderr.txt'
        stderr_text = stderr_file.read_text(encoding='utf-8', errors='replace') if stderr_file.exists() else ''
        stderr_text += result.get('stderr_tail', '')
        elf = inputs / meta['elf']
        stage = stage_of(result, True, exe is not None)
        entry = {
            'id': meta['id'],
            'label': meta['id'],
            'program_type': args.program_type,
            'provenance': args.provenance,
            'input_sha256': sha256(elf) if elf.exists() else None,
            'native_binary_sha256': sha256(exe) if exe else None,
            'stage_reached': stage,
            'stage_name': STAGE_NAMES[stage] if stage >= 0 else 'not_recognized',
            'untested_stages': STAGE_NAMES[stage + 1:],
            'failure_class': classify(result, stderr_text) if result else 'test_harness_failure',
            'reference': {'kind': 'real-PS2 expected output', 'status': result.get('status'),
                          'matching_lines': result.get('matching_lines'), 'expected_lines': result.get('expected_lines'),
                          'first_divergence': first_divergence(result)},
            'exit_code': result.get('exit_code'),
            'timings': {'recompile_seconds': meta.get('recompile_seconds'), 'run_seconds': result.get('run_seconds')},
            'rendering': 'untested', 'input': 'untested', 'audio': 'untested', 'save_load': 'untested',
            'deterministic': None,
        }
        if args.repeat_runner and exe and elf.exists() and (d / 'run.tty.txt').exists():
            again = repeat_tty(exe, elf)
            entry['deterministic'] = None if again is None else again == (d / 'run.tty.txt').read_text(encoding='utf-8', errors='replace')
        matrix['programs'].append(entry)
    summary = {}
    for p in matrix['programs']:
        summary.setdefault(p['stage_name'], 0)
        summary[p['stage_name']] += 1
    matrix['summary'] = {'programs': len(matrix['programs']), 'by_stage': summary,
                         'reference_pass': sum(1 for p in matrix['programs'] if p['reference']['status'] == 'PASS'),
                         'by_failure_class': {c: sum(1 for p in matrix['programs'] if p['failure_class'] == c)
                                              for c in FAILURE_CLASSES if any(p['failure_class'] == c for p in matrix['programs'])}}
    problems = validate_matrix(matrix)
    if problems:
        sys.exit('matrix failed validation:\n  ' + '\n  '.join(problems))
    pathlib.Path(args.out).write_text(json.dumps(matrix, indent=1) + '\n', encoding='utf-8')
    if args.md:
        write_md(matrix, pathlib.Path(args.md))
    print(f"{len(matrix['programs'])} programs; by stage {summary}; reference PASS {matrix['summary']['reference_pass']}")


def write_md(matrix, path):
    lines = [f"# Compatibility matrix: {matrix['set']}", '',
             f"Source `{matrix['source']['commit']}` (tree clean: {matrix['source']['tree_clean']}), generated "
             f"{matrix['generated']}. Stages 5-9 are never credited by this collector.", '',
             '| program | stage reached | failure class | reference | first divergence |', '|---|---|---|---|---|']
    for p in matrix['programs']:
        ref = p['reference']
        lines.append(f"| {p['id']} | {p['stage_reached']} {p['stage_name']} | {p['failure_class']} | "
                     f"{ref['status']} {ref['matching_lines']}/{ref['expected_lines']} | "
                     f"{(ref['first_divergence'] or '').replace('|', '/')} |")
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def validate_matrix(m):
    problems = []
    if m.get('schema') != SCHEMA_VERSION:
        problems.append('schema version')
    for p in m.get('programs', []):
        pid = p.get('id', '?')
        st = p.get('stage_reached')
        if not isinstance(st, int) or st < -1 or st > 9:
            problems.append(f'{pid}: stage_reached out of range')
            continue
        if st > 4:
            problems.append(f'{pid}: stages above 4 need evidence this collector does not gather')
        if p.get('failure_class') not in FAILURE_CLASSES:
            problems.append(f"{pid}: unknown failure class {p.get('failure_class')}")
        if (p.get('failure_class') == 'none') != (p.get('reference', {}).get('status') == 'PASS'):
            problems.append(f'{pid}: failure class "none" must coincide with a reference PASS')
        for field in ('rendering', 'input', 'audio', 'save_load'):
            if p.get(field) not in ('untested', 'not_applicable', 'pass', 'fail'):
                problems.append(f'{pid}: {field} must be untested/not_applicable/pass/fail')
            if p.get(field) in ('pass', 'fail') and not p.get(f'{field}_evidence'):
                problems.append(f'{pid}: {field}={p.get(field)} without {field}_evidence')
        if st >= 2 and not p.get('native_binary_sha256'):
            problems.append(f'{pid}: native compilation credited without a binary hash')
        for text in (json.dumps(p),):
            if re.search(r'[A-Za-z]:\\\\Users\\\\|/home/[^/]+/', text):
                problems.append(f'{pid}: private path in entry')
    return problems


def selftest():
    """The validator must reject entries that claim more than a run showed."""
    good = {'id': 'x', 'stage_reached': 4, 'failure_class': 'none', 'reference': {'status': 'PASS'},
            'rendering': 'untested', 'input': 'untested', 'audio': 'untested', 'save_load': 'untested',
            'native_binary_sha256': 'ab'}
    cases = [
        ('valid entry', {}, True),
        ('stage above 4', {'stage_reached': 6}, False),
        ('rendering pass without evidence', {'rendering': 'pass'}, False),
        ('"none" without a reference PASS', {'reference': {'status': 'MISMATCH'}}, False),
        ('unknown failure class', {'failure_class': 'playable'}, False),
        ('native compilation without binary hash', {'native_binary_sha256': None}, False),
        ('private path', {'label': 'C:\\Users\\someone\\game.elf'}, False),
    ]
    failures = 0
    for name, change, ok in cases:
        entry = dict(good, **change)
        problems = validate_matrix({'schema': SCHEMA_VERSION, 'programs': [entry]})
        if (not problems) != ok:
            print(f'FAIL: {name}: {problems}')
            failures += 1
    print('selftest:', 'PASS' if not failures else f'{failures} FAILURES')
    return 1 if failures else 0


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('collect')
    c.add_argument('--hwtest-dir', required=True)
    c.add_argument('--build', required=True)
    c.add_argument('--inputs', required=True, help='directory the prepare.json "elf" paths are relative to')
    c.add_argument('--label', required=True)
    c.add_argument('--program-type', default='homebrew hardware test (ps2autotests)')
    c.add_argument('--provenance', default='unknownbrackets/ps2autotests (ISC); not commercial software')
    c.add_argument('--out', required=True)
    c.add_argument('--md')
    c.add_argument('--repeat-runner', action='store_true', help='run every program a second time (determinism)')
    sub.add_parser('selftest')
    v = sub.add_parser('validate')
    v.add_argument('matrix')
    a = ap.parse_args()
    if a.cmd == 'collect':
        collect(a)
    elif a.cmd == 'selftest':
        sys.exit(selftest())
    else:
        problems = validate_matrix(json.loads(pathlib.Path(a.matrix).read_text(encoding='utf-8')))
        print('\n'.join(problems) if problems else 'valid')
        sys.exit(1 if problems else 0)
