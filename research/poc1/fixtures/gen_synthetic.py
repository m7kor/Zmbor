#!/usr/bin/env python3
"""Generate small STRIPPED MIPS32-LE ELFs with known ground truth for Category A POCs.

Own minimal assembler + ELF32 writer (no PS2 toolchain, no game code, no
unpublished-tree files). Deterministic: same bytes on every run.

Layout: .text at 0x00100000, .data right after .text (256-byte aligned).
No .symtab/.strtab: the analyser must recover functions from code/data alone.

Ground truth (function starts, in bytes written to <name>.truth.json):
  entry      : calls f_add, f_switch, f_jalr_caller, f_dataptr_user, then jr $ra
  f_add      : ordinary callee (JAL target, frame + $ra save)
  f_switch   : switch via an in-.data jump table; case blocks are INTRA-function
               blocks, NOT functions
  f_jalr_caller : calls f_target through a LUI/ORI materialised constant + JALR
  f_target   : JALR-constant target (a real function)
  f_dataptr_user : calls f_callback through a .data function pointer + JALR
  f_callback : data-pointer target (a real function)
  dead_func  : well-formed framed function, NEVER called or address-taken
               (a tool that reports it is reporting dead code, not a proven edge)
  bare_leaf  : bare `jr $ra` fragment, pointed to by ONE isolated .data word
               (Zmbor's pinned rule: a singleton pointer to a trivial leaf is
               NOT promoted to a function; a tool that reports it is a false
               positive under Zmbor's validation rules)
Only starts in the `functions` list may be described as promotable.
"""
import json
import pathlib
import struct
import sys

TEXT_BASE = 0x00100000

REGS = {n: i for i, n in enumerate(
    ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3',
     't4', 't5', 't6', 't7', 's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
     't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra'])}


def R(rs, rt, rd, sa, funct):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct


def I(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def J(op, target):
    return (op << 26) | ((target >> 2) & 0x3FFFFFF)


def NOP():
    return 0


def JR(rs):
    return R(REGS[rs], 0, 0, 0, 0x08)


def JALR(rs, rd=31):
    return R(REGS[rs], 0, rd, 0, 0x09)


class Asm:
    def __init__(self):
        self.text = []          # words or ('label',) placeholders resolved later
        self.labels = {}
        self.fixups = []        # (word_index, kind, arg)
        self.data_words = []    # (name, value-or-label)
        self.data_labels = {}

    @property
    def pc(self):
        return TEXT_BASE + 4 * len(self.text)

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = self.pc

    def w(self, word):
        self.text.append(word & 0xFFFFFFFF)

    def lui(self, rt, imm):
        self.w(I(0x0F, 0, REGS[rt], imm))

    def ori(self, rt, rs, imm):
        self.w(I(0x0D, REGS[rs], REGS[rt], imm))

    def addiu(self, rt, rs, imm):
        self.w(I(0x09, REGS[rs], REGS[rt], imm))

    def addu(self, rd, rs, rt):
        self.w(R(REGS[rs], REGS[rt], REGS[rd], 0, 0x21))

    def sll(self, rd, rt, sa):
        self.w(R(0, REGS[rt], REGS[rd], sa, 0x00))

    def sltiu(self, rt, rs, imm):
        self.w(I(0x0B, REGS[rs], REGS[rt], imm))

    def lw(self, rt, off, rs):
        self.w(I(0x23, REGS[rs], REGS[rt], off))

    def sw(self, rt, off, rs):
        self.w(I(0x2B, REGS[rs], REGS[rt], off))

    def beq(self, rs, rt, label):
        idx = len(self.text)
        self.text.append(0)
        self.fixups.append((idx, 'branch', label))

    def jal(self, label):
        idx = len(self.text)
        self.text.append(0)
        self.fixups.append((idx, 'jal', label))

    def jr(self):
        self.w(JR('ra'))

    def jalr(self, rs):
        self.w(JALR(rs))

    def nop(self):
        self.w(0)

    def li(self, rt, val):
        # addiu with small value (all our immediates fit)
        self.addiu(rt, 'zero', val)

    def la(self, rt, data_name):
        """Materialise a .data address with LUI + ADDIU; fixup after layout."""
        idx = len(self.text)
        self.text.append(0)
        self.text.append(0)
        self.fixups.append((idx, 'la', data_name))

    def data_word(self, name, value):
        self.data_labels[name] = len(self.data_words) * 4
        self.data_words.append(value)

    def build(self):
        text_size = 4 * len(self.text)
        data_base = TEXT_BASE + ((text_size + 0xFF) & ~0xFF)
        data_addr = {n: data_base + off for n, off in self.data_labels.items()}
        # Resolve fixups.
        for idx, kind, arg in self.fixups:
            pc = TEXT_BASE + 4 * idx
            if kind == 'jal':
                self.text[idx] = J(0x03, self.labels[arg])
            elif kind == 'branch':
                off = (self.labels[arg] - (pc + 4)) // 4
                # beq $rs,$rt,off -- rs/rt were encoded as 0 placeholders; the
                # caller used beq() only via helper below, so this path is unused.
                raise AssertionError('use beq_reg instead')
            elif kind == 'la':
                addr = data_addr[arg]
                self.text[idx] = I(0x0F, 0, REGS['t9'], (addr >> 16) & 0xFFFF)
                self.text[idx + 1] = I(0x09, REGS['t9'], REGS['t9'],
                                       addr & 0xFFFF if addr < 0x8000 else
                                       (addr & 0xFFFF) - 0x10000)
                # NOTE: addiu sign-extends; recompute properly below.
                hi = (addr >> 16) & 0xFFFF
                lo = addr & 0xFFFF
                if lo >= 0x8000:
                    hi = (hi + 1) & 0xFFFF
                    lo -= 0x10000
                self.text[idx] = I(0x0F, 0, REGS['t9'], hi)
                self.text[idx + 1] = I(0x09, REGS['t9'], REGS['t9'], lo)
        # Resolve data words that name code labels.
        data = bytearray()
        for v in self.data_words:
            if isinstance(v, str):
                v = self.labels[v]
            data += struct.pack('<I', v & 0xFFFFFFFF)
        return bytes(struct.pack('<%dI' % len(self.text), *self.text)), bytes(data), data_base

    def beq_reg(self, rs, rt, label):
        idx = len(self.text)
        self.text.append(0)
        self.fixups.append((idx, 'beq', (rs, rt, label)))

    def resolve_branches(self):
        for idx, kind, arg in self.fixups:
            if kind != 'beq':
                continue
            rs, rt, label = arg
            pc = TEXT_BASE + 4 * idx
            off = (self.labels[label] - (pc + 4)) // 4
            self.text[idx] = I(0x04, REGS[rs], REGS[rt], off)


def build_program():
    a = Asm()
    # ---------------- entry ----------------
    a.label('_start')
    a.jal('f_add'); a.nop()
    a.jal('f_switch'); a.nop()
    a.jal('f_jalr_caller'); a.nop()
    a.jal('f_dataptr_user'); a.nop()
    a.jr(); a.nop()
    # ---------------- f_add ----------------
    a.label('f_add')
    a.addiu('sp', 'sp', -16); a.sw('ra', 12, 'sp')
    a.addu('v0', 'a0', 'a1')
    a.lw('ra', 12, 'sp'); a.addiu('sp', 'sp', 16)
    a.jr(); a.nop()
    # ---------------- f_switch ----------------
    a.label('f_switch')
    a.addiu('sp', 'sp', -16); a.sw('ra', 12, 'sp')
    a.sltiu('t0', 'a0', 3)
    a.beq_reg('t0', 'zero', 'sw_default'); a.nop()
    a.la('t9', 'jump_table')          # t9 = &jump_table
    a.sll('t0', 'a0', 2)
    a.addu('t9', 't9', 't0')
    a.lw('t1', 0, 't9')
    a.jalr('t1'); a.nop()              # tail-ish: cases return via jr $ra chain
    a.lw('ra', 12, 'sp'); a.addiu('sp', 'sp', 16)
    a.jr(); a.nop()
    a.label('sw_case0'); a.li('v0', 10); a.jr(); a.nop()
    a.label('sw_case1'); a.li('v0', 11); a.jr(); a.nop()
    a.label('sw_case2'); a.li('v0', 12); a.jr(); a.nop()
    a.label('sw_default'); a.li('v0', 99); a.jr(); a.nop()
    # ---------------- f_jalr_caller ----------------
    a.label('f_jalr_caller')
    a.addiu('sp', 'sp', -16); a.sw('ra', 12, 'sp')
    # LUI/ORI materialisation of f_target, then JALR (constant proven in-block)
    idx = len(a.text)
    a.text.append(0); a.text.append(0)
    a.fixups.append((idx, 'lui_ori', 'f_target'))
    a.jalr('t9'); a.nop()
    a.lw('ra', 12, 'sp'); a.addiu('sp', 'sp', 16)
    a.jr(); a.nop()
    # ---------------- f_target ----------------
    a.label('f_target')
    a.li('v0', 7); a.jr(); a.nop()
    # ---------------- f_dataptr_user ----------------
    a.label('f_dataptr_user')
    a.addiu('sp', 'sp', -16); a.sw('ra', 12, 'sp')
    a.la('t9', 'callback_ptr')
    a.lw('t1', 0, 't9')
    a.jalr('t1'); a.nop()
    a.lw('ra', 12, 'sp'); a.addiu('sp', 'sp', 16)
    a.jr(); a.nop()
    # ---------------- f_callback ----------------
    a.label('f_callback')
    a.li('v0', 9); a.jr(); a.nop()
    # ---------------- dead_func (never referenced) ----------------
    a.label('dead_func')
    a.addiu('sp', 'sp', -16); a.sw('ra', 12, 'sp')
    a.li('v0', 42)
    a.lw('ra', 12, 'sp'); a.addiu('sp', 'sp', 16)
    a.jr(); a.nop()
    # ---------------- bare_leaf (decoy) ----------------
    a.label('bare_leaf')
    a.jr(); a.nop()
    a.resolve_branches()
    # LUI/ORI fixups for f_target.
    for idx, kind, arg in a.fixups:
        if kind == 'lui_ori':
            addr = a.labels[arg]
            hi = (addr >> 16) & 0xFFFF
            lo = addr & 0xFFFF
            if lo >= 0x8000:
                hi = (hi + 1) & 0xFFFF
                lo -= 0x10000
            a.text[idx] = I(0x0F, 0, REGS['t9'], hi)
            a.text[idx + 1] = I(0x0D, REGS['t9'], REGS['t9'], lo)
    # .data: jump table (3 case targets + default is NOT in the table: the
    # bounds check branches to it directly), callback pointer, decoy pointer.
    a.data_word('jump_table', 'sw_case0')
    a.data_word('jump_table_1', 'sw_case1')
    a.data_word('jump_table_2', 'sw_case2')
    a.data_word('callback_ptr', 'f_callback')
    a.data_word('decoy_ptr', 'bare_leaf')
    text, data, data_base = a.build()
    truth = {
        'entry': a.labels['_start'],
        'functions': {k: v for k, v in a.labels.items()
                      if k not in ('sw_case0', 'sw_case1', 'sw_case2',
                                   'sw_default', 'bare_leaf')},
        'intra_function_blocks': {k: v for k, v in a.labels.items()
                                  if k.startswith('sw_')},
        'non_functions': {'bare_leaf': a.labels['bare_leaf'],
                          'decoy_data_word': data_base + a.data_labels['decoy_ptr']},
        'data': {'jump_table': data_base + a.data_labels['jump_table'],
                 'callback_ptr': data_base + a.data_labels['callback_ptr'],
                 'decoy_ptr': data_base + a.data_labels['decoy_ptr'],
                 'data_base': data_base},
        'text_base': TEXT_BASE,
        'text_size': len(text),
    }
    return text, data, data_base, truth


def write_elf(path, text, data, data_base, entry):
    # ELF32 LE, EXEC, MIPS (e_machine 8). Sections: .text, .data, .shstrtab.
    # No symtab/strtab: stripped.
    shstr = b'\x00.text\x00.data\x00.shstrtab\x00'
    ehsize, phentsize, shentsize = 52, 32, 40
    phnum, shnum = 2, 4
    text_off = ehsize + phnum * phentsize
    data_off = text_off + len(text)
    data_off = (data_off + 15) & ~15
    shstr_off = data_off + len(data)
    sh_off = (shstr_off + len(shstr) + 3) & ~3
    e = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01\x00' + b'\x00' * 8,
                    2, 8, 1, entry, ehsize, sh_off, 0, ehsize, phentsize, phnum,
                    shentsize, shnum, 3)
    assert len(e) == ehsize
    ph = b''
    ph += struct.pack('<IIIIIIII', 1, text_off, TEXT_BASE, TEXT_BASE,
                      len(text), len(text), 5, 0x1000)
    ph += struct.pack('<IIIIIIII', 1, data_off, data_base, data_base,
                      len(data), len(data), 6, 0x1000)
    blob = bytearray(e + ph)
    assert len(blob) == text_off
    blob += text
    blob += b'\x00' * (data_off - len(blob)) + data
    blob += b'\x00' * (shstr_off - len(blob)) + shstr
    blob += b'\x00' * (sh_off - len(blob))
    def sh(name, typ, flags, addr, off, size, link=0, info=0, align=1, entsize=0):
        return struct.pack('<IIIIIIIIII', name, typ, flags, addr, off, size,
                           link, info, align, entsize)
    blob += sh(0, 0, 0, 0, 0, 0)
    blob += sh(1, 1, 6, TEXT_BASE, text_off, len(text), 0, 0, 16, 0)
    blob += sh(7, 1, 3, data_base, data_off, len(data), 0, 0, 16, 0)
    blob += sh(13, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    return bytes(blob)


def main():
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    text, data, data_base, truth = build_program()
    elf = write_elf(out / 'synthetic_stripped.elf', text, data, data_base,
                    truth['entry'])
    (out / 'synthetic_stripped.elf').write_bytes(elf)
    truth_out = {k: (f'0x{v:08x}' if isinstance(v, int) else
                     {kk: f'0x{vv:08x}' for kk, vv in v.items()} if isinstance(v, dict)
                     else v)
                 for k, v in truth.items()}
    (out / 'synthetic_stripped.truth.json').write_text(
        json.dumps(truth_out, indent=2) + '\n')
    print(f'{out / "synthetic_stripped.elf"} ({len(elf)} bytes)')
    print(json.dumps(truth_out, indent=2))


if __name__ == '__main__':
    main()
