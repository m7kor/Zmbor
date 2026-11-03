#!/usr/bin/env python3
"""Capstone probe: what does stock Capstone (MIPS32/64, no R5900 mode) do
with EE-specific words, and with the COP2 macro words in Zmbor's own
vu_macro fixture? Recorded verbatim -- no ground-truth claim is made for
MMI encodings beyond their opcode field (0x1C); the point is whether
Capstone decodes them at all, and how it renders them.

Usage: python3 capstone_probe.py [vu_macro.elf]  (defaults to fixtures/)
Output: JSON on stdout.
"""
import json
import struct
import sys

from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN

WORDS = {
    # (word, note) -- opcode field is the only asserted fact for EE words.
    0x2408002A: 'addiu $t0,$zero,42 (plain MIPS III, control)',
    0x0C100000: 'jal 0x100000 (plain MIPS, control)',
    0x03E00008: 'jr $ra (plain MIPS, control)',
    0x7BA80000: 'LQ opcode 0x1E: lq $t0,0($sp) (EE, 128-bit load)',
    0x7FA80000: 'SQ opcode 0x1F: sq $t0,0($sp) (EE, 128-bit store)',
    0x70000000: 'MMI opcode 0x1C, all other fields zero',
    0x70000121: 'MMI opcode 0x1C, funct-like field 0x21 (cf. PADDUW family)',
    0x48000000: 'COP2 opcode 0x12, all other fields zero',
    0x4A000000: 'COP2 opcode 0x12 with VU-macro-ish upper bits (unverified)',
}


def dis_words(md, words, base=0x100000):
    out = []
    for w in words:
        code = struct.pack('<I', w)
        insns = list(md.disasm(code, base))
        if insns:
            i = insns[0]
            out.append({'word': f'0x{w:08x}', 'mnemonic': i.mnemonic,
                        'op_str': i.op_str})
        else:
            out.append({'word': f'0x{w:08x}', 'mnemonic': None,
                        'op_str': 'NO DECODE'})
    return out


def main():
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 + CS_MODE_LITTLE_ENDIAN)
    md.detail = False
    result = {'capstone_version': __import__('capstone').__version__,
              'probe_words': []}
    for w, note in WORDS.items():
        row = dis_words(md, [w])[0]
        row['note'] = note
        result['probe_words'].append(row)
    # vu_macro fixture: decode .text words, count NO DECODE / suspicious.
    elf_path = sys.argv[1] if len(sys.argv) > 1 else 'fixtures/vu_macro.elf'
    try:
        d = open(elf_path, 'rb').read()
        shoff = struct.unpack('<I', d[32:36])[0]
        shentsize, shnum, shstrndx = struct.unpack('<HHH', d[46:52])
        secs = []
        for i in range(shnum):
            o = shoff + i * shentsize
            name, typ, flags, addr, off, size = struct.unpack('<IIIIII', d[o:o + 24])
            secs.append({'name_off': name, 'flags': flags, 'addr': addr,
                         'off': off, 'size': size})
        so = shoff + shstrndx * shentsize
        stroff = struct.unpack('<I', d[so + 16:so + 20])[0]
        for s in secs:
            end = d.index(b'\x00', stroff + s['name_off'])
            s['name'] = d[stroff + s['name_off']:end].decode('ascii', 'replace')
        text = [s for s in secs if s['name'] == '.text'][0]
        code = d[text['off']:text['off'] + text['size']]
        insns = list(md.disasm(code, text['addr']))
        decoded = len(insns)
        # Words Capstone skipped: disasm advances over undecodable words in
        # skipdata mode only; default mode stops. Re-decode word by word.
        per_word = []
        for i in range(0, len(code), 4):
            w = struct.unpack('<I', code[i:i + 4])[0]
            one = dis_words(md, [w], text['addr'] + i)[0]
            per_word.append(one)
        failed = [p for p in per_word if p['mnemonic'] is None]
        result['vu_macro'] = {
            'elf': elf_path, 'text_addr': hex(text['addr']),
            'text_words': len(code) // 4,
            'linear_decoded': decoded,
            'per_word_failed': len(failed),
            'failed_words': failed[:20],
        }
    except FileNotFoundError:
        result['vu_macro'] = {'error': f'not found: {elf_path}'}
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
