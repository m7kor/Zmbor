# Hardware probes — inventory and workflow

Zmbor has **no physical PS2** in this environment and no ps2sdk/ps2dev toolchain. Nothing in this file is a hardware
measurement. Every probe below is `UNMEASURED`; its predictions are `MODEL_PREDICTED`.

## Evidence classes

| class | meaning | hardware evidence? |
|---|---|---|
| HARDWARE_CONFIRMED | observed on a physical PS2 by this project (`probe_lab.py ingest --source hardware --console <id>`) | yes |
| HARDWARE_REFERENCE | published real-PS2 vectors (ps2autotests `.expected`, `*_hw_vectors.inc`) — register category A | yes |
| EMULATOR_OBSERVED | PCSX2 or another emulator | **no** |
| MODEL_PREDICTED | output of `tools/semantics/*` models | **no** |
| ZMBOR_OBSERVED | Zmbor's own output | **no** |
| UNMEASURED | no observation | — |

`probe_lab.py` never promotes a class: an emulator or Zmbor observation leaves the probe `UNMEASURED`, an
incomplete observation selects no model, and `--source hardware` is refused without a console identifier. The
`hwprobes_lab_selftest` ctest checks this on synthetic data and validates the committed `manifest.json`.

## Pipeline

1. **Models** — `tools/semantics/ee_fpu_models.py` (ADD M1/M2(g)), `mul_models.py` (MUL hot-one / K-trunc),
   `vu_fmac_model.py` (VU FMAC contract). Exact integer/rational code, independent of the runtime.
2. **Disagreement search** — `tools/semantics/z3_add_models.py` (optional `z3-solver`, MIT): per exponent gap, encodes
   two ADD models as bit-vector datapaths and asks Z3 for operands where they differ. `sat` results are re-checked
   against the Python models and minimised; `unsat` proves the *models* agree for that gap (says nothing about
   hardware). Without Z3, `make_probes.py` uses a seeded random search, which can miss rare disagreements but never
   produces a false one.
3. **Probe selection** — `make_probes.py <dir>`: greedy set cover, so that every separable model pair differs on at
   least one operand pair; writes `<probe>.cpp` (ps2autotests style) and `<probe>.predictions.csv`.
4. **Manifest** — `probe_lab.py manifest`: `generated/manifest.json` with the question, register entry, models,
   SHA-256 of the model sources, build requirements, hardware status and recorded observations.
5. **Hardware stage (not run)** — build each `.cpp` inside ps2autotests with ps2sdk, run on a console (ps2link /
   uLaunchELF), capture the `"<fs> <ft> <result>"` lines.
6. **Ingest** — `probe_lab.py ingest <probe> <capture> --source hardware --console <id> --provenance "<how>"`: scores
   every model; only a complete hardware capture can select one. The register entry is then updated by hand with the
   capture attached.

## Inventory

| probe | register | question | pairs | models | status |
|---|---|---|---|---|---|
| P1 `p1_ee_add_guard_bits` | 1.2 | EE ADD.S guard bits / sticky: M1, M2 g∈{0,1,2,3,8,29} | 5 | 7 | UNMEASURED |
| P2 `p2_ee_mul_partial_products` | 1.3 | EE MUL.S partial-product truncation: hot-one vs K-trunc | 3 | 7 | UNMEASURED |
| P3 `p3_ee_rsqrt_datapath` | 1.5 | EE RSQRT.S: rounded root then divide, or reciprocal-multiply | 2 | 4 | UNMEASURED |
| P4 `p4_vu_mul_asymmetry` | 2.3 | VU0 VMUL operand-order asymmetry: exact vs hot-one | 1 | 2 | UNMEASURED |

P3's candidates come from `tools/semantics/ee_rsqrt_models.py` (register 1.5): the composition the SQRT.S/DIV.S
train sets predict (29/34, production), the best composition (33/34) and the post-hoc reciprocal datapath (34/34,
two product roundings). The FMAC overflow threshold, macro-mode MAC/STATUS flags and the EFU series rounding
(register 2.1/2.4/2.5) have no candidate set yet, so no probe exists for them.

## Z3 results (models only)

`generated/z3_ee_add_disagreement.json` (`z3_add_models.py --gaps 0-30 --models "M1,M2 g=0,M2 g=1,M2 g=2,M2 g=3,M2 g=8"`,
z3 4.13.0, 4 min 21 s): 465 (gap, pair) queries, 385 `sat`, 80 `unsat`, 0 unknown; all 385 counterexamples were
reproduced by the independent Python models (0 encoding disagreements). Structure found:

- gap 0: every model agrees (no bit is shifted out);
- M1 = M2 g whenever gap ≤ g (all shifted-out bits are kept), so a probe for g needs gaps above g;
- from gap 25 upward each model gives one of two results: M2 g with g ≥ gap − 23 keeps a non-zero remnant of the
  smaller operand and, after truncation, equals M1 (a − 1 ulp); with g < gap − 23 the operand vanishes (result a).
  So M1 = M2 g=2 at gap 25 but not 26, g=0 = g=1 from 25, g≤3 all equal from 27, M1 = g=8 at 0–8 and 25–30;
- consequently most pairs are separable only with gaps 1–24, and the large-gap rows only tell "kept or dropped".

These are properties of the candidate models, used only to choose probe operands — not hardware facts.
