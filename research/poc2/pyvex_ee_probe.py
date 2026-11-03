#!/usr/bin/env python3
"""Direct PyVEX lift probe for EE words (see section file §4).
Run: .venv-angr/bin/python pyvex_ee_probe.py  (no ELF needed)
Output: JSON. Words are from the vu_macro fixture / opcode fields only;
no MMI encoding beyond its opcode field is asserted."""
import json
import pyvex, archinfo
from archinfo import Endness
arch = archinfo.ArchMIPS32(endness=Endness.LE)
WORDS = {
    '0x48a90800': 'COP2 word from vu_macro .text',
    '0x4be20928': 'COP2 word from vu_macro .text',
    '0x7ba80000': 'LQ opcode 0x1E',
    '0x7d0a0020': 'SQ opcode 0x1F (word from vu_macro .text)',
    '0x70000121': 'MMI opcode 0x1C',
    '0x2408002a': 'addiu control',
}
out = {}
for hexw, note in WORDS.items():
    w = int(hexw, 16)
    try:
        irsb = pyvex.IRSB(w.to_bytes(4, 'little'), 0x100000, arch)
        out[hexw] = {'note': note, 'size': irsb.size,
                     'n_statements': len(irsb.statements),
                     'jumpkind': str(irsb.jumpkind),
                     'statements': [str(s) for s in irsb.statements][:4]}
    except Exception as e:
        out[hexw] = {'note': note, 'error': f'{type(e).__name__}: {e}'}
print(json.dumps(out, indent=2))
