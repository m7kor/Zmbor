# Phase 8 — roadmap

Written at the end of Phase 7 (head of `ultimate/phase7-full-system`). Every item states the evidence that exists
today, what is missing, and the acceptance criterion that would let it be called done. Nothing in this document is a
result: it is a plan. Hardware correctness is claimed only from real-PS2 output; whole-game compatibility is not
claimed at all.

Starting point (Phase 7 gate): ps2autotests 53/55 programs, 8202/8250 lines; open divergences `cpu_ee_fpu_sqrt`
(59/66, RSQRT.S) and `vu_lower_efu` (182/223); `ps2x_tests` 551/551; ctest 54/54 (Z3 proofs executed); e2e 4/4;
`ps2xIOP` 4/4. Gap matrix: `phase7-gap-matrix.md`.

## Order and dependencies

| # | item | blocked on | can start without hardware |
|---|---|---|---|
| 1 | RSQRT.S and EFU hardware probes | a physical PS2 + ps2sdk toolchain to run probes | probe specifications, generators, scoring (yes) |
| 2 | GS / parallel-gs feasibility spike | a Vulkan-capable test host; GS reference dumps | trace capture format, CPU-backend golden images (yes) |
| 3 | SPU2 minimal functional architecture | hardware audio captures for validation | design, register model, unit tests from the manual (yes) |
| 4 | SIF/IOP scheduling and synchronisation tests | — | yes |
| 5 | Homebrew compatibility stages 5–9 | open-licence homebrew corpus; input scripts | yes |
| 6 | Performance and stability gates | — | yes |

Items 4 and 6 are fully verifiable locally and should go first; 1 and 3 produce designs and tooling now and results
only when hardware is available.

## 1. RSQRT.S and EFU hardware probe specifications

**Evidence today.** RSQRT.S: 34 hardware rows (ps2autotests `cpu/ee_fpu/sqrt.expected`); the pre-registered
composition SQRT∘DIV fits at most 30/34; a datapath `a * R(1/R(sqrt b))` fits 34/34 but was chosen after seeing the
rows (`tools/semantics/ee_rsqrt_models.py`). Probe P3 (`tools/hwprobes/generated/p3_ee_rsqrt_datapath.*`) already
lists operand pairs on which the candidate models disagree. EFU: 41 of 223 lines differ, last-bit errors in
EATAN/ESIN/EEXP-family results (Phase 3 model fitted from 208 hardware vectors); no EFU probe exists.

**Work.**
- P3 review: confirm every operand pair separates at least two surviving models; add pairs that separate the 34/34
  post-hoc datapath from the composition models; freeze predictions before any run (hash recorded in the manifest).
- P6 (new): EFU probe. For each EFU op, operand sets that (a) sweep the polynomial's segment boundaries, (b) hit
  exponent edges, (c) repeat the 41 failing inputs with ±1 ulp neighbours. Predictions from each candidate model
  (coefficient sets, accumulation order, truncation point) in the same CSV format as P1–P5.
- Scoring: `probe_lab.py` scores observations against predictions; a model is adopted only if it was fixed before
  the run and matches every probe row and every existing `.expected` row.

**Acceptance.** Probes build with ps2sdk and run on a real PS2; observations stored with provenance
(`evidence-register.md`); `cpu_ee_fpu_sqrt` 66/66 and `vu_lower_efu` 223/223 only through a pre-registered model.
Without hardware: probe sources, predictions and scoring land, results stay UNMEASURED.

## 2. GS / parallel-gs feasibility and rendering integration spike

**Evidence today.** CPU rasteriser behind `GSRasterBackend` (`runtime/gs/gs_backend.h`: Initialize, Reset, Submit,
LoadClut, BeginTransfer, UploadImage, Flush, Sync); `GS::setRasterBackend` swaps it. GS unit tests and e2e frame
checks exist; no hardware GS dumps. parallel-gs is LGPL-3.0 and needs Vulkan.

**Work (spike, no refactor of the CPU path).**
1. Capture format: record the backend call stream (`GSPrimitiveBatch`, transfers, CLUT loads, syncs) to a file
   from e2e and ps2autotests GS programs.
2. Golden images: replay each trace through the CPU backend; store per-frame hashes.
3. Adapter prototype `GSParallelBackend` in a separate, optional target (dynamic linking for LGPL compliance),
   replaying the same traces.
4. Measure per trace: pixel-exact match rate vs CPU backend, first-differing primitive, frame time.

**Acceptance.** A written go/no-go with the measured match table. The CPU backend stays the default and the
reference; no GS behaviour is claimed hardware-correct from agreement between two software renderers.

## 3. SPU2 minimal functional architecture

**Evidence today.** Audio is HLE (`Kernel/Stubs/Audio.cpp`, 90 TODO markers, named `sceSd*`/`sceSSyn_*` stubs). No
SPU2 register model, no ADPCM decoder, no mixer.

**Minimal architecture.**
- Register file for both cores (voice registers, ADSR, key on/off, transfer registers, IRQ address) at the IOP
  addresses, with read-back semantics.
- 2 MB sound RAM; IOP DMA channels 4/7 and manual transfer into it.
- VAG/ADPCM decoder (block = 16 bytes, filter/shift header, loop flags).
- Voice engine: pitch counter, Gaussian or linear interpolation (selectable), ADSR envelope state machine.
- Mixer: 48 kHz, per-core dry/wet paths; reverb as a stub that outputs silence (documented).
- Host output behind an interface; a null sink for tests.

**Tests.** ADPCM decoder against hand-computed blocks; ADSR phase transitions; key-on to first sample; DMA into
sound RAM; IRQ on address match. Validation against hardware captures is a later step.

**Acceptance.** Unit tests for each block; `sceSd*` HLE can be routed to the model behind a flag; no claim of
audio accuracy without captures.

## 4. SIF/IOP scheduling and synchronisation tests

**Evidence today.** `ps2xIOP` 4/4; SIF RPC and SIF DMA unit tests; IOP modules are HLE.

**Work.**
- Deterministic scheduler tests: EE and IOP step counts per tick fixed; SIF DMA completion ordering vs SIF RPC
  callbacks; `sceSifSetDma` / `sceSifDmaStat` polling loops terminate.
- Interleavings: two concurrent RPC clients on one server; a bind before the server registers; RPC from an interrupt
  handler; end-of-transfer interrupts while the EE is in a wait loop.
- Fuzz: a SIF command stream harness (`tools/fuzz`) for the packet-level IOP SIF responder.
- Regression: every found ordering bug becomes a deterministic test with fixed step counts.

**Acceptance.** All tests deterministic (same result over N repeated runs), part of ctest and the gate.

## 5. End-to-end homebrew compatibility stages 5–9

**Evidence today.** `tools/compat` credits stages 0–4 from real runs (55 ps2autotests programs at stage 4); stages
5–9 (interactive, main_workload, extended_execution, persistence, broad_compatibility) are never credited.

**Work.**
- Corpus: open-licence homebrew only, with licence and SHA-256 recorded; no commercial titles, BIOS or SDK
  binaries in the repository.
- Stage 5 interactive: a scripted pad-input mechanism (new work; none exists today), evidence = a state change
  observed after the scripted input.
- Stage 6 main workload: program-specific checkpoint markers and frame hashes after N frames.
- Stage 7 extended execution: N minutes without crash, with memory and frame-time bounds.
- Stage 8 persistence: memory-card write, restart, read back.
- Stage 9 broad: only as an aggregate of 5–8 across the corpus, never per title from one run.
- `compat.py` validator: each stage requires its own evidence field; a stage is never inferred from another.

**Acceptance.** Each credited stage links to the run log, input script and hashes that produced it.

## 6. Performance and stability acceptance gates

**Evidence today.** `tools/bench/ps2x_bench_vu1` (A/B method in `benchmark-methodology.md`), sampling profiler
(Windows), gate manifests with binary hashes; CI runs the bench as a correctness check only.

**Work.**
- Performance gate: fixed workloads (VU1 transform, XGKICK, GIF-heavy trace, DMA chain replay), 5 alternating
  rounds base vs head on one machine; report median and spread; fail on a regression beyond the measured noise
  band; never compare CI runner timings.
- Real-time-relative reporting for programs: emulated cycles per host second against the PS2 clock.
- Stability gate: fuzz smoke (CI, 60 s per target) plus a longer local run per release (≥ 30 min per target)
  with corpus kept outside the repo; ASan+UBSan unit/e2e on every PR; the gate reports skipped tests explicitly.

**Acceptance.** Gate output includes the performance table and stability results, with the comparison anchor
recorded.

## Out of scope until evidence exists

Claims of hardware-accurate GS, SPU2 or IOP timing; commercial game compatibility; adopting any fitted model that
was chosen after seeing the rows it is scored on.
