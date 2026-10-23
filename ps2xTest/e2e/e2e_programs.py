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


EXPECTED['gs_sprite'] = 'E2E gs frame submitted'

# GS registers and packets (GS User's Manual register numbers; GIFtag layout: NLOOP 0-14, EOP 15, FLG 58-59,
# NREG 60-63, REGS in the high doubleword; PACKED A+D = register descriptor 0xE).
GS_PRIM, GS_RGBAQ, GS_XYZ2, GS_XYOFFSET_1, GS_PRMODECONT = 0x00, 0x01, 0x05, 0x18, 0x1A
GS_SCISSOR_1, GS_TEST_1, GS_FRAME_1, GS_ZBUF_1 = 0x40, 0x47, 0x4C, 0x4E
BACKGROUND = (0x20, 0x40, 0x80)
SPRITE = (0xFF, 0x00, 0x00)
SPRITE_RECT = (100, 100, 300, 200)   # x0, y0, x1, y1: pixels x0 <= x < x1, y0 <= y < y1 are covered


def gif_ad_packet(regs):
    import struct
    tag_lo = len(regs) | (1 << 15) | (0 << 58) | (1 << 60)   # NLOOP, EOP, PACKED, NREG = 1
    out = struct.pack('<QQ', tag_lo, 0xE)
    for addr, data in regs:
        out += struct.pack('<QQ', data & 0xFFFFFFFFFFFFFFFF, addr)
    return out


def rgbaq(r, g, b, a=0x80):
    return r | (g << 8) | (b << 16) | (a << 24)


def xyz2(x, y):
    return (x << 4) | ((y << 4) << 16)   # 12.4 fixed point, XYOFFSET 0, Z 0


def program_gs_sprite():
    import struct
    a = Assembler()

    a.function('_start')
    a.i('lui', 'sp', 'zero', 0x01F0)
    a.jump('jal', 'main')
    a.nop()
    a.jump('jal', 'exit')
    a.r('or', rd='a0', rs='zero', rt='zero')
    a.label('hang')
    a.branch('beq', 'zero', 'zero', 'hang')
    a.nop()

    a.function('main')
    a.i('addiu', 'sp', 'sp', -16)
    a.i('sw', 'ra', 'sp', 12)

    # Display: circuit 1 shows a 640x448 PSMCT32 framebuffer at FBP 0 (PMODE.EN1, CRTMD=1, MMOD with ALP=0xFF).
    a.li('t0', 0x12000000)
    a.li64('t1', 0xFF25, 't9')
    a.i('sd', 't1', 't0', 0x00)                                   # PMODE
    a.li64('t1', 10 << 9, 't9')
    a.i('sd', 't1', 't0', 0x70)                                   # DISPFB1: FBW 10 (640), PSMCT32, FBP 0
    a.li64('t1', (3 << 23) | (2559 << 32) | (447 << 44), 't9')
    a.i('sd', 't1', 't0', 0x80)                                   # DISPLAY1: MAGH 3, DW 2559, DH 447

    # PATH3: GIF DMA (channel 2, normal mode) with the drawing environment and a full-screen background sprite.
    a.li('t0', 0x1000A000)
    a.la('t1', 'gif_packet')
    a.i('sw', 't1', 't0', 0x10)                                   # D2_MADR
    a.li('t1', len(GIF_PACKET) // 16)
    a.i('sw', 't1', 't0', 0x20)                                   # D2_QWC
    a.li('t1', 0x101)
    a.i('sw', 't1', 't0', 0x00)                                   # D2_CHCR: STR, from memory

    # PATH2: VIF1 DMA (channel 1) whose DIRECT payload draws the red sprite.
    a.li('t0', 0x10009000)
    a.la('t1', 'vif_packet')
    a.i('sw', 't1', 't0', 0x10)                                   # D1_MADR
    a.li('t1', len(VIF_PACKET) // 16)
    a.i('sw', 't1', 't0', 0x20)                                   # D1_QWC
    a.li('t1', 0x101)
    a.i('sw', 't1', 't0', 0x00)                                   # D1_CHCR: STR, from memory

    a.la('a0', 's_done')
    a.jump('jal', 'puts')
    a.nop()

    # Keep running long enough for the host to present several frames, then return.
    a.li('t0', 0x02000000)
    a.label('wait_loop')
    a.i('addiu', 't0', 't0', -1)
    a.branch('bne', 't0', 'zero', 'wait_loop')
    a.nop()

    a.i('lw', 'ra', 'sp', 12)
    a.r('jr', rs='ra')
    a.i('addiu', 'sp', 'sp', 16)

    for name in ('puts', 'exit'):
        a.function(name)
        a.r('jr', rs='ra')
        a.nop()

    a.data_block('gif_packet', GIF_PACKET)
    a.data_block('vif_packet', VIF_PACKET)
    a.data_block('s_done', EXPECTED['gs_sprite'].encode() + b'\0', align=4)
    return a, ['puts', 'exit']


GIF_PACKET = gif_ad_packet([
    (GS_FRAME_1, 10 << 16),                                       # FBP 0, FBW 10, PSMCT32
    (GS_SCISSOR_1, (639 << 16) | (447 << 48)),
    (GS_XYOFFSET_1, 0),
    (GS_TEST_1, (1 << 16) | (1 << 17)),                           # ZTE on, ZTST ALWAYS
    (GS_ZBUF_1, 0x100 | (1 << 32)),                               # ZBP 0x100, depth writes masked
    (GS_PRMODECONT, 1),
    (GS_PRIM, 6),                                                 # sprite
    (GS_RGBAQ, rgbaq(*BACKGROUND)),
    (GS_XYZ2, xyz2(0, 0)),
    (GS_XYZ2, xyz2(640, 448)),
])
_SPRITE_GIF = gif_ad_packet([
    (GS_PRIM, 6),
    (GS_RGBAQ, rgbaq(*SPRITE)),
    (GS_XYZ2, xyz2(SPRITE_RECT[0], SPRITE_RECT[1])),
    (GS_XYZ2, xyz2(SPRITE_RECT[2], SPRITE_RECT[3])),
])
VIF_PACKET = (__import__('struct').pack('<IIII', 0, 0, 0, (0x50 << 24) | (len(_SPRITE_GIF) // 16)) + _SPRITE_GIF)

# Pixels the presented frame must have (interior points, away from edges whose rasterization rules are not under
# test here), computed from the geometry above rather than stored in the runtime.
FRAME_EXPECTATIONS = {
    'gs_sprite': {
        'width': 640, 'height': 448,
        'pixels': [[20, 20, BACKGROUND], [630, 440, BACKGROUND], [99 - 9, 150, BACKGROUND], [310, 150, BACKGROUND],
                   [110, 110, SPRITE], [200, 150, SPRITE], [290, 190, SPRITE]],
    },
}

PROGRAMS = {'basic': program_basic, 'gs_sprite': program_gs_sprite}


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
    if name in FRAME_EXPECTATIONS:
        import json
        (out_dir / f'{name}.frame.json').write_text(json.dumps(FRAME_EXPECTATIONS[name]), encoding='utf-8')
    print(f'{elf_path} ({elf_path.stat().st_size} bytes)')


if __name__ == '__main__':
    main()
