#!/usr/bin/env python3
"""IOP hardware result tests: run the ps2autotests IOP programs (<name>.irx with a sibling <name>.expected printed by
a real PS2) on the ps2xIOP emulator and compare their console output with the expected text.

  iop_hwtest.py --tests <ps2autotests/tests> --runner <ps2_iop_autotest_runner> --out <dir>
                [--json <file>] [--md <file>] [--baseline <json>] [--filter <substring>]...

Each program runs through `hwtest.py run` (same BEGIN/END extraction, comparison and statuses as the EE programs);
the dashboard is written by `hwtest.py report`. The IOP programs are a separate measurement: they are not added to
the EE program counts. The .expected files are reference data; this tool never rewrites them.
"""
import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import hwtest  # noqa: E402  (sibling module, path set above because the tool runs with python -I)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tests', required=True)
    ap.add_argument('--runner', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--json')
    ap.add_argument('--md')
    ap.add_argument('--baseline')
    ap.add_argument('--filter', action='append')
    ap.add_argument('--timeout', type=float, default=60)
    a = ap.parse_args(argv)

    tests_root = pathlib.Path(a.tests).resolve()
    out = pathlib.Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    programs = sorted(p for p in tests_root.rglob('*.irx') if p.with_suffix('.expected').exists())
    if a.filter:
        programs = [p for p in programs if any(f in p.as_posix() for f in a.filter)]
    failures = 0
    for irx in programs:
        tid = hwtest.test_id(tests_root, irx)
        work = out / tid
        work.mkdir(parents=True, exist_ok=True)
        rel = irx.relative_to(tests_root)
        hwtest.write_json(work / 'prepare.json', {'id': tid, 'elf': rel.as_posix(), 'subsystem': hwtest.subsystem_of(rel)})
        run_args = argparse.Namespace(runner=a.runner, elf=str(irx), expected=str(irx.with_suffix('.expected')),
                                      result=str(work / 'result.json'), timeout=a.timeout)
        failures += 1 if hwtest.run(run_args) != 0 else 0
    hwtest.report(argparse.Namespace(out=str(out), json=a.json, md=a.md, baseline=a.baseline))
    print(f'{len(programs) - failures}/{len(programs)} IOP programs match their real-PS2 output')
    return 0


if __name__ == '__main__':
    sys.exit(main())
