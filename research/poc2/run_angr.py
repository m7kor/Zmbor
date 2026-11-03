#!/usr/bin/env python3
"""POC 2 oracle: angr CFGFast on a PS2 ELF, scored against ground truth with
Zmbor's promotion rules (same rules as poc1/validate_poc1.py, re-implemented
here so poc2 stays standalone).

What this measures, and what it does NOT claim:
  - CFGFast function starts are CANDIDATES. A start becomes PROMOTABLE only
    if it is 4-aligned, inside an executable section, not a delay slot, and
    (for the synthetic fixture) in the truth `functions` set. Symbolic /
    VSA-derived edges are reported separately and are NEVER promoted by this
    script: Zmbor promotion requires executable-section, alignment, range,
    precedence and CFG validation, and a solver guess satisfies none of
    those by itself.
  - angr lifts via VEX (MIPS32/MIPS64 only). EE MMI (opcode 0x1C), LQ/SQ,
    COP2/VU macro and 128-bit register operations have NO VEX lifter: blocks
    containing them are expected to fail lifting or truncate. That failure
    is a first-class result of this POC, not a reason to retry with
    different flags until something looks plausible.
  - PS2 ELFs are NOT Linux binaries: there is no loader, no libc, no entry
    stub. CFGFast is therefore run with auto_load_libs=False and the ELF
    entry point as the start; syscall-heavy code will stop at the first
    unmodelled syscall. Report that honestly.

Usage:
  python3 run_angr.py <elf> [--truth truth.json] [--timeout SECONDS]
Output: JSON on stdout + a human-readable summary on stderr.
Requires: pip install angr  (see run_poc2.sh; large install, needs several
GB free and a working C toolchain for pyvex).
"""
import json
import resource
import struct
import sys
import time


def parse_sections(path):
    d = open(path, 'rb').read()
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
    return d, secs


def main():
    elf = sys.argv[1]
    truth_path = sys.argv[sys.argv.index('--truth') + 1] if '--truth' in sys.argv else None
    t0 = time.time()
    import angr  # noqa: E402  (import here so --help-style failures stay clean)

    data, secs = parse_sections(elf)
    exec_secs = [s for s in secs if s['flags'] & 0x4 and s['size'] > 0]

    def word_at(addr):
        for s in exec_secs:
            if s['addr'] <= addr < s['addr'] + s['size']:
                o = s['off'] + (addr - s['addr'])
                return struct.unpack('<I', data[o:o + 4])[0]
        return None

    def is_control(word):
        op = word >> 26
        if op in (0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
            return not (op == 0x01 and ((word >> 16) & 0x1F) >= 0x18)
        return op == 0x00 and (word & 0x3F) in (0x08, 0x09)

    proj = angr.Project(elf, auto_load_libs=False)
    cfg = proj.analyses.CFGFast()
    reported = sorted(cfg.functions.keys())
    lift_errors = 0
    # CORRECTION (2026-11-03, measured): counting blocks whose .vex is None
    # does NOT detect EE lift failure. VEX returns an IRSB object even for
    # words it cannot decode: empty statement list + Ijk_NoDecode (COP2,
    # LQ), Ijk_SigILL (SQ), or a bare IMark with Ijk_Boring and no data
    # flow (MMI -- silently dropped semantics, the worst case). So count
    # blocks whose IRSB is empty or NoDecode/SigILL. A block that mixes
    # good and EE instructions still counts as 0 here if any statement
    # exists; this metric is therefore a LOWER bound on EE damage.
    try:
        for addr in reported:
            fn = cfg.functions[addr]
            for block in fn.blocks:
                vex = getattr(block, 'vex', None)
                if vex is None:
                    lift_errors += 1
                else:
                    try:
                        stmts = vex.statements
                        jk = str(vex.jumpkind)
                        if len(stmts) == 0 or 'NoDecode' in jk or 'SigILL' in jk:
                            lift_errors += 1
                    except Exception:
                        pass
    except Exception:
        pass

    if truth_path:
        truth = json.load(open(truth_path))
        expected = {int(v, 16): k for k, v in truth['functions'].items()}
        nonfunc = {int(v, 16): k for k, v in truth['non_functions'].items()}
        blocks = {int(v, 16): k for k, v in truth['intra_function_blocks'].items()}
    else:
        expected, nonfunc, blocks = {}, {}, {}

    tp, fp, cand, missed = [], [], [], []
    for addr in reported:
        prev = word_at(addr - 4) if addr >= 4 else None
        ok = (addr % 4 == 0
              and any(s['addr'] <= addr < s['addr'] + s['size'] for s in exec_secs)
              and not (prev is not None and is_control(prev)))
        if addr in expected and ok:
            tp.append(addr)
        elif addr in nonfunc or addr in blocks:
            fp.append(addr)
        else:
            cand.append(addr)
    for addr in expected:
        if addr not in reported:
            missed.append(addr)
    result = {
        'tool': 'angr CFGFast',
        'angr_version': getattr(angr, '__version__', 'unknown'),
        'elf': elf,
        'arch': str(proj.arch),
        'entry': hex(proj.entry),
        'reported': len(reported),
        'reported_addrs': [hex(a) for a in reported],
        'promotable_tp': [hex(a) for a in tp],
        'false_positive': [hex(a) for a in fp],
        'candidate_only': [hex(a) for a in cand],
        'missed': [hex(a) for a in missed],
        'unresolved_indirect': sorted(
            hex(a) for a in getattr(cfg, 'unresolved_indirect_jumps', []) or []),
        'blocks_empty_or_nodecode_vex': lift_errors,
        'wall_seconds': round(time.time() - t0, 2),
        'peak_rss_mb': round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024, 1),
    }
    print(json.dumps(result, indent=2))
    print(f"angr={result['angr_version']} arch={result['arch']} "
          f"reported={len(reported)} TP={len(tp)} FP={len(fp)} "
          f"candidate={len(cand)} missed={len(missed)} "
          f"time={result['wall_seconds']}s rss={result['peak_rss_mb']}MB",
          file=sys.stderr)


if __name__ == '__main__':
    main()
