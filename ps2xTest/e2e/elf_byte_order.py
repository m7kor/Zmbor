"""Check ELF format and load-segment admission through both command-line tools."""

import json
import pathlib
import struct
import subprocess
import sys
import tempfile


def write_elf(path, order, encoding, memory_size=8, file_size=8):
    base = 0x00100000
    offset = 0x100
    ident = b'\x7fELF' + bytes([1, encoding, 1]) + bytes(9)
    header = struct.pack(order + 'HHIIIIIHHHHHH', 2, 8, 1, base, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    # One executable PT_LOAD with JR $ra and its NOP delay slot.
    segment = struct.pack(order + '8I', 1, offset, base, base, file_size, memory_size, 5, offset)
    instructions = struct.pack(order + 'II', 0x03E00008, 0)
    path.write_bytes((ident + header + segment).ljust(offset, b'\x00') + instructions)


def run(tool, args, work, expected_status, diagnostic=None):
    result = subprocess.run([tool, *args], cwd=work, capture_output=True, text=True, timeout=20)
    output = result.stdout + result.stderr
    if result.returncode != expected_status or (diagnostic and diagnostic not in output):
        raise AssertionError(f'{pathlib.Path(tool).name} {args}: exit {result.returncode}\n{output}')
    return output


def check_section_coverage(analyzer, recompiler, work):
    cases = [
        ('full_coverage', 16, 16, []),
        ('full_zero_fill', 8, 16, []),
        ('partial_code', 4, 4, ['4 of 8 bytes', '0 of 8 bytes']),
        ('partial_data', 12, 12, ['4 of 8 bytes']),
        ('overlapping_loads', 8, 8, ['4 of 8 bytes']),
        ('no_coverage', 0, 0, ['0 of 8 bytes', '0 of 8 bytes']),
    ]
    for label, file_size, memory_size, expected in cases:
        elf = work / f'{label}.elf'
        write_elf(elf, '<', 1, memory_size, file_size)
        image = bytearray(elf.read_bytes()) + bytes(8)
        if label == 'overlapping_loads':
            struct.pack_into('<H', image, 44, 2)  # e_phnum
            struct.pack_into('<8I', image, 52 + 32, 1, 0x104, 0xA0100004, 0xA0100004, 8, 8, 5, 1)
        struct.pack_into('<I', image, 32, len(image))  # e_shoff
        struct.pack_into('<H', image, 48, 3)  # e_shnum
        image.extend(bytes(40))
        image.extend(struct.pack('<10I', 0, 1, 6, 0x00100000, 0x100, 8, 0, 0, 4, 0))
        image.extend(struct.pack('<10I', 0, 1, 3, 0x00100008, 0x108, 8, 0, 0, 4, 0))
        elf.write_bytes(image)
        (work / f'{label}.toml').write_text(
            f'[general]\ninput = "{label}.elf"\noutput = "{label}_generated"\n', encoding='utf-8'
        )
        for tool, args in [(analyzer, [elf.name, f'{label}_analysis.toml']), (recompiler, [f'{label}.toml'])]:
            output = run(tool, args, work, 0)
            warnings = [line for line in output.splitlines() if 'covered by initial PT_LOAD' in line]
            if len(warnings) != len(expected) or any(part not in line for part, line in zip(expected, warnings)):
                raise AssertionError(f'{pathlib.Path(tool).name} {label}: unexpected coverage warnings\n{output}')
        if label == 'partial_code':
            check_load_correlation(output, work)


def check_load_correlation(analysis, work):
    (work / 'analysis.log').write_text(analysis, encoding='utf-8')
    (work / 'trace.txt').write_text(
        '== zmbor trace summary ==\nelapsed_ms=1 enabled_mask=16 keys=1 events=3\n'
        'elf:sif-segment 4 0x00000000 0x00000000 1 4 | last\n'
        '-- ordered events (first 3 per key)\n'
        '#1 t=0.1ms cat=16 pc=0x0 elf:sif-segment vaddr=0xa0100006 region=rdram offset=0x00100006 filesz=2 memsz=4\n'
        '#2 t=0.2ms cat=16 pc=0x0 elf:sif-segment vaddr=0x00100008 region=rdram offset=0x00100008 filesz=4 memsz=4\n'
        '#3 t=0.3ms cat=16 pc=0x0 elf:sif-segment vaddr=0xf0000000 region=scratchpad offset=0x00000000 filesz=4 memsz=4\n',
        encoding='utf-8',
    )
    tool = pathlib.Path(__file__).resolve().parents[2] / 'tools' / 'compat' / 'correlate_loads.py'
    report = json.loads(run(sys.executable, [str(tool), 'analysis.log', 'trace.txt'], work, 0))
    observed = [(gap['range'], gap['observed_write_bytes']) for gap in report['gaps']]
    if (observed != [('rdram:0x100004-0x100008', 2), ('rdram:0x100008-0x100010', 4)]
            or not report['load_trace_enabled'] or not report['segment_details_incomplete']
            or report['segment_writes_detailed'] != 3):
        raise AssertionError(f'unexpected load correlation: {report}')

    (work / 'trace.txt').write_text(
        '== zmbor trace summary ==\nelapsed_ms=1 enabled_mask=0 keys=0 events=0\n'
        '-- ordered events (first 4 per key)\n', encoding='utf-8'
    )
    report = json.loads(run(sys.executable, [str(tool), 'analysis.log', 'trace.txt'], work, 0))
    if report['load_trace_enabled'] or any(gap['observed_write_bytes'] is not None for gap in report['gaps']):
        raise AssertionError(f'disabled tracing should leave coverage unknown: {report}')

    (work / 'trace.txt').write_text(
        '== zmbor trace summary ==\nelapsed_ms=1 enabled_mask=16 keys=1 events=2\n'
        'elf:sif-segment 2 0x00000000 0x00000000 1 2 | last\n'
        '-- ordered events (first 4 per key)\n'
        '#1 t=0.1ms cat=16 pc=0x0 elf:sif-segment vaddr=0x00100004 region=rdram offset=0x00100004'
        ' filesz=4 memsz=4 fnv1a64=4d25767f9dce13f5\n'
        '#2 t=0.2ms cat=16 pc=0x0 elf:sif-segment vaddr=0x00100005 region=rdram offset=0x00100005'
        ' filesz=1 memsz=1 fnv1a64=af63bc4c8601b62c\n', encoding='utf-8'
    )
    args = [str(tool), 'analysis.log', 'trace.txt', '--elf', 'partial_code.elf']
    report = json.loads(run(sys.executable, args, work, 0))
    first = report['gaps'][0]
    if (first['observed_write_bytes'] != 4 or first['last_sif_write_fingerprint_matches'] is not None
            or first['sif_gap_fingerprint_matches'] is not None):
        raise AssertionError(f'later partial write should leave the content comparison unknown: {report}')

    incorrect_elf = bytearray((work / 'partial_code.elf').read_bytes())
    incorrect_elf[0x104] = 1
    (work / 'wrong_gap_bytes.elf').write_bytes(incorrect_elf)
    run(sys.executable, [*args[:-1], 'wrong_gap_bytes.elf'], work, 2, 'ELF bytes disagree with analysis')

    trace_path = work / 'trace.txt'
    trace_path.write_text(trace_path.read_text(encoding='utf-8').split('#2', 1)[0], encoding='utf-8')
    report = json.loads(run(sys.executable, args, work, 0))
    if (not report['segment_details_incomplete']
            or report['gaps'][0]['last_sif_write_fingerprint_matches'] is not None
            or report['gaps'][0]['sif_gap_fingerprint_matches'] is not None):
        raise AssertionError(f'a capped trace must not certify the last write: {report}')


def main(analyzer, recompiler):
    with tempfile.TemporaryDirectory(prefix='zmbor-elf-byte-order-') as tmp:
        work = pathlib.Path(tmp)
        fixtures = [('lsb', '<', 1, 16, 8), ('msb', '>', 2, 8, 8),
                    ('short_mem', '<', 1, 4, 8), ('zero_mem', '<', 1, 0, 8),
                    ('truncated', '<', 1, 8, 8), ('oversized', '<', 1, 512, 512),
                    ('missing_code', '<', 1, 8, 8), ('missing_data', '<', 1, 8, 8)]
        for label, order, encoding, memory_size, file_size in fixtures:
            elf = work / f'{label}.elf'
            write_elf(elf, order, encoding, memory_size, file_size)
            if label == 'truncated':
                elf.write_bytes(elf.read_bytes()[:-1])
            if label in ('missing_code', 'missing_data'):
                image = bytearray(elf.read_bytes())
                struct.pack_into('<I', image, 32, len(image))  # e_shoff
                struct.pack_into('<H', image, 48, 2)  # e_shnum
                image.extend(bytes(40))  # null section
                flags = 6 if label == 'missing_code' else 3
                image.extend(struct.pack('<10I', 0, 1, flags, 0x00100000, 0x100, 8192, 0, 0, 4, 0))
                elf.write_bytes(image)
            (work / f'{label}.toml').write_text(
                f'[general]\ninput = "{label}.elf"\noutput = "{label}_generated"\n'
                'single_file_output = true\nstubs = []\nskip = []\n',
                encoding='utf-8',
            )

        run(analyzer, ['lsb.elf', 'lsb_analysis.toml'], work, 0)
        run(recompiler, ['lsb.toml'], work, 0)
        generated = work / 'lsb_generated' / 'ps2_recompiled_functions.cpp'
        if not (work / 'lsb_analysis.toml').is_file() or 'JR $ra' not in generated.read_text(encoding='utf-8'):
            raise AssertionError('the little-endian ELF should produce analysis and translate JR $ra')

        for label, diagnostic in [('msb', 'Expected a little-endian PS2 ELF file'),
                                  ('short_mem', 'ELF segment 0 has filesz > memsz'),
                                  ('zero_mem', 'ELF segment 0 has filesz > memsz'),
                                  ('truncated', 'Failed to parse ELF file'),
                                  ('oversized', 'ELF segment 0 has no file payload'),
                                  ('missing_code', 'has no file payload'),
                                  ('missing_data', 'has no file payload')]:
            run(analyzer, [f'{label}.elf', f'{label}_analysis.toml'], work, 1, 'Failed to parse ELF file')
            run(recompiler, [f'{label}.toml'], work, 1, diagnostic)
            if (work / f'{label}_analysis.toml').exists() or list((work / f'{label}_generated').glob('*.cpp')):
                raise AssertionError(f'the rejected ELF {label} should not produce analysis or code')

        check_section_coverage(analyzer, recompiler, work)


if __name__ == '__main__':
    main(*sys.argv[1:])
