#!/usr/bin/env python3
"""Minimal R5900 (MIPS III + EE extensions) assembler and ELF32 writer for end-to-end tests.

Test programs are written as Python calls (see e2e_programs.py) so the default test suite needs no PS2 toolchain
and no copyrighted code. Only the instructions the tests use are encoded; each encoding follows the MIPS IV / EE
Core instruction set layout (opcode 31-26, rs 25-21, rt 20-16, rd 15-11, sa 10-6, funct 5-0).
"""
import struct

REGS = {name: i for i, name in enumerate(
    ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
     's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra'])}

R_FUNCT = {'sll': 0x00, 'srl': 0x02, 'sra': 0x03, 'jr': 0x08, 'jalr': 0x09, 'syscall': 0x0C, 'mfhi': 0x10,
           'mflo': 0x12, 'mult': 0x18, 'multu': 0x19, 'div': 0x1A, 'divu': 0x1B, 'addu': 0x21, 'subu': 0x23,
           'and': 0x24, 'or': 0x25, 'xor': 0x26, 'nor': 0x27, 'slt': 0x2A, 'sltu': 0x2B, 'daddu': 0x2D,
           'dsll': 0x38, 'dsrl': 0x3A, 'dsra': 0x3B, 'dsll32': 0x3C, 'dsrl32': 0x3E, 'dsra32': 0x3F}
I_OP = {'beq': 0x04, 'bne': 0x05, 'blez': 0x06, 'bgtz': 0x07, 'addiu': 0x09, 'slti': 0x0A, 'sltiu': 0x0B,
        'andi': 0x0C, 'ori': 0x0D, 'lui': 0x0F, 'lq': 0x1E, 'sq': 0x1F, 'lb': 0x20, 'lh': 0x21, 'lw': 0x23,
        'lbu': 0x24, 'lhu': 0x25, 'sb': 0x28, 'sh': 0x29, 'sw': 0x2B, 'ld': 0x37, 'sd': 0x3F,
        'beql': 0x14, 'bnel': 0x15}
REGIMM_RT = {'bltz': 0x00, 'bgez': 0x01}


def reg(r):
    return REGS[r] if isinstance(r, str) else int(r)


class Assembler:
    """Two-section program: .text at text_base, .data right after it (16-byte aligned blocks)."""

    def __init__(self, text_base=0x00100000):
        self.text_base = text_base
        self.text = []            # list of (word or callable(label_addr) -> word)
        self.labels = {}          # name -> address
        self.functions = []       # (name, start, end)
        self.data = bytearray()
        self.data_labels = {}
        self._fn = None

    # ------------------------------------------------------------------ layout
    @property
    def pc(self):
        return self.text_base + 4 * len(self.text)

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = self.pc

    def function(self, name):
        if self._fn:
            self.functions.append((self._fn[0], self._fn[1], self.pc))
        self._fn = (name, self.pc)
        self.label(name)

    def end_functions(self):
        if self._fn:
            self.functions.append((self._fn[0], self._fn[1], self.pc))
            self._fn = None

    def data_block(self, name, payload, align=16):
        while len(self.data) % align:
            self.data.append(0)
        self.data_labels[name] = len(self.data)
        self.data += payload

    def data_base(self):
        size = 4 * len(self.text)
        return self.text_base + ((size + 0xFF) & ~0xFF)

    def addr(self, name):
        if name in self.labels:
            return self.labels[name]
        return self.data_base() + self.data_labels[name]

    # ------------------------------------------------------------------ encoders
    def word(self, w):
        self.text.append(w & 0xFFFFFFFF)

    def r(self, op, rd=0, rs=0, rt=0, sa=0):
        self.word((reg(rs) << 21) | (reg(rt) << 16) | (reg(rd) << 11) | ((sa & 31) << 6) | R_FUNCT[op])

    def i(self, op, rt, rs, imm):
        self.word((I_OP[op] << 26) | (reg(rs) << 21) | (reg(rt) << 16) | (imm & 0xFFFF))

    def nop(self):
        self.word(0)

    def la(self, rt, name):
        """lui/ori pair resolved after layout (ori avoids the sign-extension adjustment of addiu)."""
        idx = len(self.text)
        self.text.append(lambda a=self, n=name, t=rt: (I_OP['lui'] << 26) | (reg(t) << 16) | (a.addr(n) >> 16))
        self.text.append(lambda a=self, n=name, t=rt: (I_OP['ori'] << 26) | (reg(t) << 21) | (reg(t) << 16) | (a.addr(n) & 0xFFFF))
        return idx

    def li(self, rt, value):
        value &= 0xFFFFFFFF
        if value < 0x8000:
            self.i('addiu', rt, 'zero', value)
        else:
            self.i('lui', rt, 'zero', value >> 16)
            if value & 0xFFFF:
                self.i('ori', rt, rt, value & 0xFFFF)

    def li64(self, rt, value, tmp):
        """Full 64-bit constant: high word, shift, then the zero-extended low word ORed in (clobbers tmp)."""
        value &= 0xFFFFFFFFFFFFFFFF
        hi, lo = value >> 32, value & 0xFFFFFFFF
        self.li(rt, hi)
        self.r('dsll32', rd=rt, rt=rt, sa=0)
        self.li(tmp, lo)
        self.r('dsll32', rd=tmp, rt=tmp, sa=0)
        self.r('dsrl32', rd=tmp, rt=tmp, sa=0)
        self.r('or', rd=rt, rs=rt, rt=tmp)

    def branch(self, op, rs, rt_or_target, target=None):
        pc = self.pc
        if op in REGIMM_RT:
            dest, rt = rt_or_target, REGIMM_RT[op]
            opcode = 0x01
        else:
            dest, rt = target, reg(rt_or_target)
            opcode = I_OP[op]
        self.text.append(lambda a=self, p=pc, d=dest, o=opcode, s=rs, t=rt:
                         (o << 26) | (reg(s) << 21) | (t << 16) | (((a.addr(d) - (p + 4)) >> 2) & 0xFFFF))

    def jump(self, op, target):
        opcode = 0x02 if op == 'j' else 0x03
        self.text.append(lambda a=self, o=opcode, d=target: (o << 26) | ((a.addr(d) >> 2) & 0x03FFFFFF))

    # ------------------------------------------------------------------ ELF
    def elf(self, entry='_start'):
        self.end_functions()
        text = b''.join(struct.pack('<I', (w(self) if callable(w) else w) & 0xFFFFFFFF) for w in self.text)
        data_base = self.data_base()
        image = text + bytes(data_base - self.text_base - len(text)) + bytes(self.data)

        strtab = bytearray(b'\0')
        syms = [struct.pack('<IIIBBH', 0, 0, 0, 0, 0, 0)]
        for name, start, end in self.functions:
            off = len(strtab)
            strtab += name.encode() + b'\0'
            syms.append(struct.pack('<IIIBBH', off, start, end - start, (1 << 4) | 2, 0, 1))  # GLOBAL FUNC .text
        for name, off_in_data in self.data_labels.items():
            off = len(strtab)
            strtab += name.encode() + b'\0'
            syms.append(struct.pack('<IIIBBH', off, data_base + off_in_data, 0, (1 << 4) | 1, 0, 2))  # OBJECT .data
        symtab = b''.join(syms)
        shstr = bytearray(b'\0')

        def sname(n):
            o = len(shstr)
            shstr.extend(n.encode() + b'\0')
            return o

        names = [sname(n) for n in ('.text', '.data', '.symtab', '.strtab', '.shstrtab')]
        image_off = 0x1000
        text_size = data_base - self.text_base
        off_symtab = image_off + len(image)
        off_strtab = off_symtab + len(symtab)
        off_shstr = off_strtab + len(strtab)
        off_sh = (off_shstr + len(shstr) + 3) & ~3
        shdrs = [bytes(40),
                 struct.pack('<10I', names[0], 1, 0x6, self.text_base, image_off, len(text), 0, 0, 16, 0),
                 struct.pack('<10I', names[1], 1, 0x3, data_base, image_off + text_size, len(self.data), 0, 0, 16, 0),
                 struct.pack('<10I', names[2], 2, 0, 0, off_symtab, len(symtab), 4, 1, 4, 16),
                 struct.pack('<10I', names[3], 3, 0, 0, off_strtab, len(strtab), 0, 0, 1, 0),
                 struct.pack('<10I', names[4], 3, 0, 0, off_shstr, len(shstr), 0, 0, 1, 0)]
        ehdr = (b'\x7fELF' + bytes([1, 1, 1, 0]) + bytes(8) +
                struct.pack('<HHIIIIIHHHHHH', 2, 8, 1, self.addr(entry), 52, off_sh, 0, 52, 32, 1, 40, len(shdrs), 5))
        phdr = struct.pack('<8I', 1, image_off, self.text_base, self.text_base, len(image), len(image), 7, 0x1000)
        out = bytearray(ehdr + phdr)
        out += bytes(image_off - len(out)) + image + symtab + strtab + shstr
        out += bytes(off_sh - len(out))
        for sh in shdrs:
            out += sh
        return bytes(out)
