#!/usr/bin/env python3
"""Score a Ghidra function-map CSV against known ground truth, applying
Zmbor's promotion rules (NOT Ghidra's word for it).

A reported start counts as PROMOTABLE only if ALL hold:
  - 4-byte aligned
  - inside an executable section of the ELF (parsed from section headers)
  - not the delay slot of a control-transfer instruction (previous word is a
    branch/jump/jal/jr/jalr, excluding REGIMM MTSAB/MTSAH which have no
    delay slot -- same rule as Zmbor's fallback scan)
  - for the synthetic fixture, the start is in the truth `functions` set
    (intra-function switch blocks and the bare-leaf decoy are NOT promotable,
    even if they pass the mechanical checks above)
Everything else reported is a CANDIDATE (or a FALSE POSITIVE if it is a
known non-function: a switch block or the bare leaf).

Usage:
  validate_poc1.py <elf> <map.csv> [--truth truth.json]
  Without --truth, expected functions are read from the ELF symtab FUNC
  symbols (main e2e ELFs only; this measures agreement with symbols, not
  stripped recovery, and is labelled as such in the output).
"""
import json
import struct
import subprocess
import sys

CONTROL_OPS = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17}


def parse_sections(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF' and d[4] == 1, 'need ELF32'
    le = d[5] == 1
    en = '<' if le else '>'
    shoff = struct.unpack(en + 'I', d[32:36])[0]
    shentsize, shnum, shstrndx = struct.unpack(en + 'HHH', d[46:52])
    secs = []
    for i in range(shnum):
        o = shoff + i * shentsize
        name, typ, flags, addr, off, size = struct.unpack(en + 'IIIIII', d[o:o + 24])
        secs.append({'name_off': name, 'type': typ, 'flags': flags,
                     'addr': addr, 'off': off, 'size': size})
    so = shoff + shstrndx * shentsize
    stroff = struct.unpack(en + 'I', d[so + 16:so + 20])[0]
    for s in secs:
        end = d.index(b'\x00', stroff + s['name_off'])
        s['name'] = d[stroff + s['name_off']:end].decode('ascii', 'replace')
    return d, secs


def is_control_transfer(word):
    op = word >> 26
    if op in CONTROL_OPS:
        if op == 0x01 and (word >> 16) & 0x1F >= 0x18:  # MTSAB/MTSAH: no delay slot
            return False
        return True
    if op == 0x00 and (word & 0x3F) in (0x08, 0x09):  # JR / JALR
        return True
    return False


def main():
    elf, csv_path = sys.argv[1], sys.argv[2]
    truth_path = None
    if '--truth' in sys.argv:
        truth_path = sys.argv[sys.argv.index('--truth') + 1]
    data, secs = parse_sections(elf)
    exec_secs = [s for s in secs if s['flags'] & 0x4 and s['size'] > 0]

    def word_at(addr):
        for s in exec_secs:
            if s['addr'] <= addr < s['addr'] + s['size']:
                o = s['off'] + (addr - s['addr'])
                return struct.unpack('<I', data[o:o + 4])[0]
        return None

    def in_exec(addr):
        return any(s['addr'] <= addr < s['addr'] + s['size'] for s in exec_secs)

    def is_delay_slot(addr):
        prev = word_at(addr - 4)
        return prev is not None and is_control_transfer(prev)

    reported = {}
    with open(csv_path) as f:
        header = f.readline()
        assert header.strip() == 'Name,Start,End,Size', f'bad header: {header!r}'
        for line in f:
            line = line.strip()
            if not line:
                continue
            name, start, end, size = line.split(',')
            reported[int(start, 0)] = (name, int(end, 0), int(size, 0))

    if truth_path:
        truth = json.load(open(truth_path))
        expected = {int(v, 16): k for k, v in truth['functions'].items()}
        nonfunc = {int(v, 16): k for k, v in truth['non_functions'].items()}
        blocks = {int(v, 16): k for k, v in truth['intra_function_blocks'].items()}
        mode = 'stripped ground truth'
    else:
        out = subprocess.run(['readelf', '-sW', elf], capture_output=True,
                             text=True, check=True).stdout
        expected, nonfunc, blocks = {}, {}, {}
        for line in out.splitlines():
            parts = line.split()
            # Num: Value Size Type Bind Vis Ndx Name
            if len(parts) >= 8 and parts[3] == 'FUNC' and parts[6] != 'UND':
                expected[int(parts[1], 16)] = parts[7]
        mode = 'ELF symtab FUNC symbols (NOT stripped recovery)'

    tp, fp, cand, missed = [], [], [], []
    for addr, (name, end, size) in sorted(reported.items()):
        ok = (addr % 4 == 0 and in_exec(addr) and not is_delay_slot(addr))
        if addr in expected and ok:
            tp.append((addr, name))
        elif addr in nonfunc or addr in blocks:
            fp.append((addr, name, 'known non-function'))
        elif not ok:
            cand.append((addr, name, 'fails promotion rule'))
        else:
            cand.append((addr, name, 'passes mechanical rules, not in truth'))
    for addr, name in sorted(expected.items()):
        if addr not in reported:
            missed.append((addr, name))
    print(f'mode: {mode}')
    print(f'reported={len(reported)} promotable_TP={len(tp)} '
          f'false_positive={len(fp)} candidate={len(cand)} missed={len(missed)}')
    for tag, rows in (('TP', tp), ('FP', fp), ('CAND', cand), ('MISSED', missed)):
        for r in rows:
            print(f'  {tag} 0x{r[0]:08x} {r[1:]}')
    print(json.dumps({'mode': mode, 'reported': len(reported),
                      'tp': [hex(a) for a, _ in tp],
                      'fp': [hex(a) for a, *_ in fp],
                      'candidate': [hex(a) for a, *_ in cand],
                      'missed': [hex(a) for a, _ in missed]}))


if __name__ == '__main__':
    main()
