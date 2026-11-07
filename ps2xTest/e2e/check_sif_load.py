"""Verify guest SIF writes and correlate the real trace with its initial ELF gaps."""

import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def run(args, work, env=None):
    result = subprocess.run(args, cwd=work, env=env, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f'{args[0]} exited {result.returncode}\n{result.stdout}\n{result.stderr}')
    return result.stdout + '\n' + result.stderr


def main(runner, analyzer, work):
    work = Path(work)
    image = work / 'sif_load.elf'
    overlay = (work / 'sif_overlay.elf').read_bytes()
    alias, = struct.unpack_from('<I', overlay, 52 + 8)
    offset = alias & 0x1FFFFFFF
    tool = Path(__file__).resolve().parents[2] / 'tools' / 'compat' / 'correlate_loads.py'
    with tempfile.TemporaryDirectory(prefix='sif-load-', dir=work) as tmp:
        tmp = Path(tmp)
        coverage = tmp / 'coverage.log'
        trace = tmp / 'trace.txt'
        analysis = run([analyzer, str(image), str(tmp / 'analysis.toml')], work)
        coverage.write_text(analysis, encoding='utf-8')
        env = dict(os.environ, PS2X_HIDDEN_WINDOW='1', ZMBOR_TRACE_LOAD='1',
                   ZMBOR_TRACE_EVENTS='4', ZMBOR_SUMMARY_FILE=str(trace))
        output = run([runner, str(image)], work, env)
        expected = (work / 'sif_load.expected').read_text(encoding='utf-8').strip()
        if expected not in output.splitlines():
            raise AssertionError(f'guest did not confirm the payload and zero-fill\n{output}')
        summary = trace.read_text(encoding='utf-8')
        detail = f'vaddr=0x{alias:08x} region=rdram offset=0x{offset:08x} filesz=4 memsz=8'
        if f'{detail} fnv1a64=70de197232004835' not in summary:
            raise AssertionError(f'missing SIF alias write\n{summary}')
        report = json.loads(run([sys.executable, '-I', str(tool), str(coverage), str(trace)], work))
        gaps = [(gap['range'], gap['observed_write_bytes'], gap['last_sif_write_fingerprint_matches'])
                for gap in report['gaps']]
        if (gaps != [(f'rdram:0x{offset:x}-0x{offset + 8:x}', 8, True)]
                or report['segment_writes_counted'] != 1 or report['segment_writes_detailed'] != 1
                or report['segment_details_incomplete'] or not report['load_trace_enabled']):
            raise AssertionError(f'unexpected correlation: {report}')

        # The same destination and extent can contain the wrong bytes.
        temp_image = tmp / image.name
        temp_image.write_bytes(image.read_bytes())
        (tmp / 'sif_overlay.elf').write_bytes(overlay[:-4] + struct.pack('<I', 0x87654321))
        bad_trace = tmp / 'wrong-payload.txt'
        bad_output = run([runner, str(temp_image)], tmp, dict(env, ZMBOR_SUMMARY_FILE=str(bad_trace)))
        bad_report = json.loads(run([sys.executable, '-I', str(tool), str(coverage), str(bad_trace)], work))
        if ('E2E SIF payload check failed' not in bad_output.splitlines()
                or expected in bad_output.splitlines()
                or f'{detail} fnv1a64=4eb123627446530d' not in bad_trace.read_text(encoding='utf-8')
                or [(gap['range'], gap['observed_write_bytes'], gap['last_sif_write_fingerprint_matches'])
                    for gap in bad_report['gaps']] != [(f'rdram:0x{offset:x}-0x{offset + 8:x}', 8, False)]):
            raise AssertionError(f'guest did not distinguish a wrong payload at the same range\n{bad_output}')
        header = bytearray(overlay[:52])
        struct.pack_into('<H', header, 44, 2)
        payload_offset = len(header) + 2 * struct.calcsize('<8I')
        segments = (struct.pack('<8I', 1, payload_offset, alias, alias, 4, 4, 6, 1)
                    + struct.pack('<8I', 1, payload_offset + 4, alias + 4, alias + 4, 0, 4, 6, 1))
        for payload, matches in ((overlay[-4:], True), (struct.pack('<I', 0x87654321), False)):
            (tmp / 'sif_overlay.elf').write_bytes(header + segments + payload)
            split_trace = tmp / f'split-{matches}.txt'
            split_output = run([runner, str(temp_image)], tmp, dict(env, ZMBOR_SUMMARY_FILE=str(split_trace)))
            split_report = json.loads(run([sys.executable, '-I', str(tool), str(coverage), str(split_trace),
                                           '--elf', str(temp_image)], work))
            if ((expected in split_output.splitlines()) != matches
                    or split_report['segment_writes_counted'] != 2
                    or split_report['segment_writes_detailed'] != 2
                    or [(gap['observed_write_bytes'], gap['last_sif_write_fingerprint_matches'],
                         gap['sif_gap_fingerprint_matches']) for gap in split_report['gaps']]
                    != [(8, None, matches)]):
                raise AssertionError(f'unexpected split payload result: {split_report}\n{split_output}')
        print(f'{expected}. Checked matching and different payloads in one or two SIF writes.')


if __name__ == '__main__':
    main(*sys.argv[1:])
