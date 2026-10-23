#!/usr/bin/env python3
"""End-to-end test programs: build a guest ELF and its recompiler config.

Usage: e2e_programs.py <program> <out_dir>
Writes <out_dir>/<program>.elf and <out_dir>/<program>.toml; the runner prints the expected line on stdout.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from mips_elf import Assembler  # noqa: E402

# Each program prints exactly this line through the guest's own puts() when every checked behaviour is right.
EXPECTED = {
    'basic': 'E2E sum=5050 fact=3628800 b63=1 lq=1',
}


def emit_string_helpers(a):
    # strcpy_end(a0 = dst, a1 = src) -> v0 = dst after the copied bytes (no terminator)
    a.function('strcpy_end')
    a.label('sc_loop')
    a.i('lb', 't0', 'a1', 0)
    a.branch('beq', 't0', 'zero', 'sc_done')
    a.nop()
    a.i('sb', 't0', 'a0', 0)
    a.i('addiu', 'a0', 'a0', 1)
    a.branch('beq', 'zero', 'zero', 'sc_loop')
    a.i('addiu', 'a1', 'a1', 1)              # delay slot
    a.label('sc_done')
    a.r('jr', rs='ra')
    a.r('or', rd='v0', rs='a0', rt='zero')   # delay slot

    # utoa(a0 = dst, a1 = value) -> v0 = dst after the digits; digits built on the stack, then reversed
    a.function('utoa')
    a.i('addiu', 'sp', 'sp', -16)
    a.r('or', rd='t1', rs='sp', rt='zero')
    a.i('addiu', 't2', 'zero', 10)
    a.label('ut_digit')
    a.r('divu', rs='a1', rt='t2')
    a.r('mfhi', rd='t3')
    a.r('mflo', rd='a1')
    a.i('addiu', 't3', 't3', 48)
    a.i('sb', 't3', 't1', 0)
    a.branch('bne', 'a1', 'zero', 'ut_digit')
    a.i('addiu', 't1', 't1', 1)              # delay slot
    a.label('ut_copy')
    a.i('addiu', 't1', 't1', -1)
    a.i('lb', 't3', 't1', 0)
    a.i('sb', 't3', 'a0', 0)
    a.branch('bne', 't1', 'sp', 'ut_copy')
    a.i('addiu', 'a0', 'a0', 1)              # delay slot
    a.i('addiu', 'sp', 'sp', 16)
    a.r('jr', rs='ra')
    a.r('or', rd='v0', rs='a0', rt='zero')   # delay slot


def program_basic():
    a = Assembler()

    a.function('_start')
    a.i('lui', 'sp', 'zero', 0x01F0)         # own stack at 0x01F00000
    a.jump('jal', 'main')
    a.nop()
    a.jump('jal', 'exit')
    a.r('or', rd='a0', rs='zero', rt='zero')
    a.label('hang')
    a.branch('beq', 'zero', 'zero', 'hang')
    a.nop()

    a.function('main')
    a.i('addiu', 'sp', 'sp', -48)
    for off, r in ((44, 'ra'), (40, 's0'), (36, 's1'), (32, 's2'), (28, 's3'), (24, 's4')):
        a.i('sw', r, 'sp', off)

    # 1) loop: sum 1..100, stored to memory and read back
    a.li('t0', 0)
    a.li('t1', 1)
    a.li('t2', 101)
    a.label('sum_loop')
    a.r('addu', rd='t0', rs='t0', rt='t1')
    a.i('addiu', 't1', 't1', 1)
    a.branch('bne', 't1', 't2', 'sum_loop')
    a.nop()
    a.la('t3', 'var')
    a.i('sw', 't0', 't3', 0)
    a.i('lw', 's0', 't3', 0)

    # 2) recursion with a stack frame per call: fact(10)
    a.li('a0', 10)
    a.jump('jal', 'fact')
    a.nop()
    a.r('or', rd='s1', rs='v0', rt='zero')

    # 3) signed branch on a 64-bit register whose low word is 0 and bit 63 is set (R5900 tests bit 63)
    a.i('lui', 't5', 'zero', 0x8000)
    a.r('dsll32', rd='t5', rt='t5', sa=0)
    a.li('s2', 0)
    a.branch('bltz', 't5', 'b63_set')
    a.nop()
    a.branch('beq', 'zero', 'zero', 'b63_done')
    a.nop()
    a.label('b63_set')
    a.li('s2', 1)
    a.label('b63_done')

    # 4) LQ ignores the low four address bits: lq from qbuf+4 must read qbuf[0..15]
    a.la('t7', 'qbuf')
    a.i('lq', 't6', 't7', 4)
    a.la('t8', 'qbuf2')
    a.i('sq', 't6', 't8', 0)
    a.i('lw', 't9', 't8', 0)
    a.li('t4', 0x11111111)
    a.r('subu', rd='t9', rs='t9', rt='t4')
    a.i('sltiu', 's3', 't9', 1)

    # format "E2E sum=%u fact=%u b63=%u lq=%u" in guest code, then puts()
    a.la('s4', 'outbuf')
    for text_label, value_reg in (('s_sum', 's0'), ('s_fact', 's1'), ('s_b63', 's2'), ('s_lq', 's3')):
        a.r('or', rd='a0', rs='s4', rt='zero')
        a.la('a1', text_label)
        a.jump('jal', 'strcpy_end')
        a.nop()
        a.r('or', rd='a0', rs='v0', rt='zero')
        a.r('or', rd='a1', rs=value_reg, rt='zero')
        a.jump('jal', 'utoa')
        a.nop()
        a.r('or', rd='s4', rs='v0', rt='zero')
    a.i('sb', 'zero', 's4', 0)
    a.la('a0', 'outbuf')
    a.jump('jal', 'puts')
    a.nop()

    for off, r in ((24, 's4'), (28, 's3'), (32, 's2'), (36, 's1'), (40, 's0'), (44, 'ra')):
        a.i('lw', r, 'sp', off)
    a.r('jr', rs='ra')
    a.i('addiu', 'sp', 'sp', 48)             # delay slot

    # fact(a0): a0 < 2 ? 1 : a0 * fact(a0 - 1)
    a.function('fact')
    a.i('slti', 't0', 'a0', 2)
    a.branch('beq', 't0', 'zero', 'fact_rec')
    a.nop()
    a.r('jr', rs='ra')
    a.li('v0', 1)                            # delay slot
    a.label('fact_rec')
    a.i('addiu', 'sp', 'sp', -16)
    a.i('sw', 'ra', 'sp', 12)
    a.i('sw', 'a0', 'sp', 8)
    a.jump('jal', 'fact')
    a.i('addiu', 'a0', 'a0', -1)             # delay slot
    a.i('lw', 'a0', 'sp', 8)
    a.r('mult', rs='a0', rt='v0')
    a.r('mflo', rd='v0')
    a.i('lw', 'ra', 'sp', 12)
    a.r('jr', rs='ra')
    a.i('addiu', 'sp', 'sp', 16)             # delay slot

    emit_string_helpers(a)

    # libc entry points the runtime provides as stubs (bound by name in the config)
    for name in ('puts', 'exit'):
        a.function(name)
        a.r('jr', rs='ra')
        a.nop()

    a.data_block('var', bytes(16))
    a.data_block('qbuf', bytes.fromhex('11111111' '22222222' '33333333' '44444444'))
    a.data_block('qbuf2', bytes(16))
    a.data_block('outbuf', bytes(96))
    for label, text in (('s_sum', 'E2E sum='), ('s_fact', ' fact='), ('s_b63', ' b63='), ('s_lq', ' lq=')):
        a.data_block(label, text.encode() + b'\0', align=4)
    return a, ['puts', 'exit']


PROGRAMS = {'basic': program_basic}


def main():
    name, out_dir = sys.argv[1], pathlib.Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    asm, stubs = PROGRAMS[name]()
    elf_path = out_dir / f'{name}.elf'
    elf_path.write_bytes(asm.elf())
    gen_dir = out_dir / f'{name}_generated'
    stub_list = ', '.join(f'"{s}"' for s in stubs)
    (out_dir / f'{name}.toml').write_text(
        '[general]\n'
        f'input = "{elf_path.as_posix()}"\n'
        f'output = "{gen_dir.as_posix()}"\n'
        'single_file_output = true\n'
        f'stubs = [{stub_list}]\n'
        'skip = []\n', encoding='utf-8')
    (out_dir / f'{name}.expected').write_text(EXPECTED[name] + '\n', encoding='utf-8')
    print(f'{elf_path} ({elf_path.stat().st_size} bytes)')


if __name__ == '__main__':
    main()
