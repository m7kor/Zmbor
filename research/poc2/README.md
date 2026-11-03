# POC 2 — angr CFGFast as secondary oracle (+ Capstone decode probe)

## Status at delivery (2026-11-03)

- **angr: RUN AND MEASURED (2026-11-03).** `pip install angr`
  completed in `.venv-angr` (`ANGR_INSTALL_EXIT=0` in
  `angr_install.log`): angr 10.0.5, pyvex 10.0.5, z3-solver 5.3.1.0.
  Results in `results_synthetic.json`, `results_basic.json`,
  `results_vu_macro.json` and `pyvex_ee_probe_results.json`:
  synthetic 12 reported / 8 TP / 4 FP / 0 missed (5.2 s warm,
  133.6 MB); basic 10 reported / 7 of 7 symtab / 3 extra;
  vu_macro 9 reported / 7 of 7 symtab / 2 extra. PyVEX: COP2/LQ
  NoDecode, SQ SigILL, MMI silent no-op. Full analysis in the
  section file §4; do not quote these numbers without the FP and
  VEX caveats.
- **Capstone probe: RUN AND MEASURED.** Capstone 5.0.9 (pip wheel;
  module `__version__` 5.0.7) was installed into `.venv-capstone` and
  `capstone_probe.py` was run on EE probe words + the vu_macro
  fixture. Results: `capstone_probe_results.json`. Summary: plain
  MIPS decodes correctly; `lq` → `xori.b` (wrong), `sq` → `ext`
  (wrong) or NO DECODE, MMI → `madd` (wrong) or NO DECODE, VU COP2
  words → NO DECODE; linear decode of vu_macro `.text` stops after
  13 words; per-word failures 13/227 (12 COP2, 1 SQ). Capstone is
  therefore a differential decoder for the MIPS subset ONLY — never
  an R5900 oracle. This probe does not substitute for angr CFGFast.
- **No Ghidra output has been produced or claimed (that is POC 1).**

## Exact commands (angr, once installed)

```bash
cd poc2
./run_poc2.sh fixtures/synthetic_stripped.elf
# or, with an existing venv:
.venv-angr/bin/python run_angr.py fixtures/synthetic_stripped.elf \
  --truth fixtures/synthetic_stripped.truth.json
.venv-angr/bin/python run_angr.py fixtures/basic.elf
.venv-angr/bin/python run_angr.py fixtures/vu_macro.elf
```

## Exact commands (Capstone probe, already run)

```bash
cd poc2
python3 -m venv .venv-capstone
.venv-capstone/bin/pip install capstone
.venv-capstone/bin/python capstone_probe.py fixtures/vu_macro.elf \
  | tee capstone_probe_results.json
```

## Files

- `run_angr.py` — CFGFast oracle. Reports function starts, promotable
  TP / false positives / candidates / missed under Zmbor's promotion
  rules, `unresolved_indirect_jumps`, `blocks_without_vex`, wall time,
  peak RSS, as JSON. Symbolic/VSA edges are NEVER promoted by this
  script. TESTED: the three `results_*.json` files in this directory
  are its output.
- `run_poc2.sh` — venv + `pip install angr` + run. Warns about the
  install size/time in its header. (The venv was in fact created
  manually during this task; re-running the script reinstalls into
  the same venv.)
- `capstone_probe.py` + `capstone_probe_results.json` — measured
  Capstone behaviour (see Status above).
- `fixtures/` — copies of poc1's fixtures (four main e2e ELFs +
  synthetic stripped fixture + truth JSON).
- `.venv-capstone/` — Capstone venv (local, not a deliverable).
- `.venv-angr/` (when complete) — angr venv (local, large, not a
  deliverable). `angr_install.log` — install attempt log.

## Scoring rules (what counts)

Same as POC 1: only starts passing alignment + executable-section +
delay-slot checks AND in the truth `functions` set are promotable.
angr-resolved indirect targets that fail those checks are candidates.
A function angr misses because its block failed VEX lifting is
reported via `blocks_without_vex` — it must not be read as "function
does not exist". PS2 ELFs are not Linux binaries: no loader, no libc,
unmodelled EE syscalls — symbolic execution stopping at a syscall
boundary is an expected limitation, not a bug to work around by
inventing a syscall model.
