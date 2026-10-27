#!/usr/bin/env python3
"""Write small hand-built seed inputs for the tools/fuzz harnesses (docs/architecture/fuzzing.md).

Every seed is constructed here from the format definitions; no game, BIOS or SDK data is involved. Seeds only give
the fuzzer valid structure to mutate from; they assert nothing.

usage: make_seeds.py <out dir>      (creates <out>/{gif,vif0,vif1,dma,elf,decoder}/seed_*.bin)
"""
import struct
import sys
from pathlib import Path


def giftag(nloop, eop, flg, nreg, regs, pre=0, prim=0):
    lo = (nloop & 0x7FFF) | (eop << 15) | (pre << 46) | ((prim & 0x7FF) << 47) | (flg << 58) | (nreg << 60)
    return struct.pack("<QQ", lo, regs)


def gif_seeds():
    packed = giftag(1, 1, 0, 3, 0x541, pre=1, prim=0x6)  # PRIM sprite; regs RGBAQ(1), XYZ2(5), XYZ2(4->5)
    packed += struct.pack("<IIII", 0x80, 0x40, 0x20, 0x80)            # RGBAQ
    packed += struct.pack("<IIII", 0x8000, 0x8000, 0, 0)              # XYZ2
    packed += struct.pack("<IIII", 0x8400, 0x8400, 0, 0)              # XYZ2
    reglist = giftag(2, 1, 1, 2, 0x51) + struct.pack("<QQQQ", 0x80408020, 0x80008000, 0x80408020, 0x84008400)
    image = giftag(1, 0, 0, 4, 0xEEEE) + struct.pack("<QQ", 0, 0x50)  # A+D: BITBLTBUF
    image += struct.pack("<QQ", 0, 0x51) + struct.pack("<QQ", (4 << 32) | 4, 0x52) + struct.pack("<QQ", 0, 0x53)
    image += giftag(4, 1, 2, 0, 0) + bytes(range(64))
    return {"packed": packed, "reglist": reglist, "image": image}


def vif(cmd, num=0, imm=0):
    return struct.pack("<I", (cmd << 24) | ((num & 0xFF) << 16) | (imm & 0xFFFF))


def vif_seeds(vif1):
    base = vif(0x01, 0, 0x0404) + vif(0x05, 0, 0) + vif(0x6C, 1, 0) + struct.pack("<ffff", 1.0, 2.0, 3.0, 1.0)
    mpg = vif(0x4A, 2, 0) + struct.pack("<II", 0x000002FF, 0x8000033C) * 2   # NOP|NOP pairs, upper E bit later
    seeds = {"unpack": base, "mpg_mscal": mpg + vif(0x14, 0, 0), "nops": vif(0) * 4 + vif(0x10) + vif(0x11)}
    if vif1:
        seeds["direct"] = vif(0x50, 0, 2) + giftag(1, 1, 0, 1, 0xE) + struct.pack("<QQ", 0x0, 0x3F)
    return seeds


def dma_seeds():
    # 4-byte tag address (offset into the copied window) then RDRAM contents at 0x100000.
    end_tag = struct.pack("<QQ", 1 | (7 << 28), 0)                      # END, QWC=1, followed by one data qword
    payload = giftag(0, 1, 0, 0, 0)
    ref_tag = struct.pack("<QQ", 1 | (3 << 28) | (0x100020 << 32), 0)   # REF to 0x100020, QWC=1
    refs_tag = struct.pack("<QQ", 1 | (4 << 28) | (0x100020 << 32), 0)  # REFS to 0x100020, QWC=1
    return {"end": struct.pack("<I", 0) + end_tag + payload,
            "ref_then_end": struct.pack("<I", 0) + ref_tag + end_tag + payload,
            # control bit 1: stall control with D_STADR = 0x100000, so the REFS tag waits until the harness moves it.
            "refs_stall": struct.pack("<I", 2) + refs_tag + end_tag + payload}


def elf_seed():
    text = struct.pack("<IIII", 0x27BDFFF0, 0x03E00008, 0x27BD0010, 0)  # addiu sp; jr ra; addiu sp; nop
    shstr = b"\0.text\0.shstrtab\0"
    ehsize, phsize, shsize = 52, 32, 40
    text_off = ehsize + phsize
    shstr_off = text_off + len(text)
    sh_off = (shstr_off + len(shstr) + 3) & ~3
    eh = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    eh += struct.pack("<HHIIIIIHHHHHH", 2, 8, 1, 0x00100000, ehsize, sh_off, 0x20924001, ehsize, phsize, 1,
                      shsize, 3, 2)
    ph = struct.pack("<IIIIIIII", 1, text_off, 0x00100000, 0x00100000, len(text), len(text), 5, 16)
    body = eh + ph + text + shstr
    body += bytes(sh_off - len(body))
    sh = bytes(shsize)
    sh += struct.pack("<IIIIIIIIII", 1, 1, 6, 0x00100000, text_off, len(text), 0, 0, 16, 0)
    sh += struct.pack("<IIIIIIIIII", 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    return {"minimal": body + sh}


def decoder_seeds():
    words = [0x27BDFFF0, 0x7BBF0000, 0x7FBF0000, 0x03E00008, 0x0C040000, 0x70000000 | (0x08 << 0) | (1 << 21),
             0x4A000038, 0x4BE0003C, 0x46000000, 0x10000002, 0x00000000, 0x0000000C, 0x42000018]
    return {"mixed": b"".join(struct.pack("<I", w) for w in words)}


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = Path(sys.argv[1])
    groups = {"gif": gif_seeds(), "vif0": vif_seeds(False), "vif1": vif_seeds(True), "dma": dma_seeds(),
              "elf": elf_seed(), "decoder": decoder_seeds()}
    for target, seeds in groups.items():
        (out / target).mkdir(parents=True, exist_ok=True)
        for name, data in seeds.items():
            (out / target / f"seed_{name}.bin").write_bytes(data)
    print(f"wrote {sum(len(s) for s in groups.values())} seeds to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
