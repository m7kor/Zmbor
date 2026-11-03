# Zmbor Integration Feasibility Matrix

Research program deliverable 2 of 4. Companion documents: `OPEN_SOURCE_TECHNOLOGY_LANDSCAPE.md`
(full landscape), `TECHNOLOGY_BENCHMARKS.md` (measurements), `../architecture/NEXT_GENERATION_RECOMPILER_DESIGN.md`
(incremental design). Section files under `~/workspace/zmbor-research/sections/` carry the full
per-candidate template entries; this matrix is the decision document, with actual evidence and
a prioritized recommendation — not a wish list.

Read the verdicts literally:

- **ADOPT** = do it; the evidence is sufficient and the cost is bounded. (Two ADOPTs carry
  explicit completion conditions — see §2 — and are not "done" until those conditions pass.)
- **PROTOTYPE** = build the smallest isolated experiment that can falsify it, behind a flag,
  and decide on the measured result. A prototype that fails its experiment is a REJECT, not
  a "phase 2".
- **STUDY** = paper/design work only, no code yet, with a named trigger for promotion.
- **DEFER** = a real candidate with a stated trigger; revisiting before the trigger is waste.
- **REJECT** = do not pursue in this form; the reason is structural (licence, architecture,
  constraint), not a lack of effort.

## 1. The matrix

Effort: S = days, M = weeks, L = months (single-maintainer scale, including tests and docs).
"Benefit metric" names what would be measured; "Falsifier" names what kills it. "Experiment"
points to the section-file design (A: E1–E7, B: E1–E8, C: E1–E8, D: E1–E5, E: C-1–C-6,
F: 8 designs) or to a POC already run.

### 1.1 ADOPT (in priority order)

| # | Candidate (category) | Effort / Risk | Benefit metric | Falsifier | Experiment / evidence |
|---|---|---|---|---|---|
| A1 | Ghidra headless oracle pipeline (A) | M / Medium (JVM + pinning + validation discipline) | Function-boundary precision on stripped fixtures: measured 8/8 TP, 0 FP on synthetic; 0 FP on all 4 e2e fixtures | A POC 1 CSV fails `loadGhidraMap`, or the label-collection port adds FP that cannot be filtered by Zmbor's validation rules | POC 1 RAN (see benchmarks). Completion conditions: (1) load a POC 1 CSV through `loadGhidraMap`; (2) port label/materialised-address/data-pointer collection and re-measure |
| A2 | sccache install + docs, local cache only (E) | S / Low (zero code; hook already exists, default ON) | Build time: ccache proxy measured 5.9× on the `ps2_recomp` subset (cold 134.0 s vs warm 22.8 s) | sccache binary unavailable on a supported host, or cache poisoning/invalidation bugs in practice | POC 3 RAN (proxy measurement; sccache itself not run — install + measure is the adoption step) |
| A3 | ccache as measured alternative (E) | S / Low (one configure flag; never both launchers) | Same as A2, measured directly | Warm-cache hit rate collapses on Zmbor's actual edit pattern | POC 3 RAN |
| A4 | ps2autotests as a CI gate (F) | M / Low-Medium (re-establish baseline; RSQRT/EFU allowlist) | Regression detection: previously silent regressions become gate failures | Baseline cannot be re-established deterministically, or the gate flakes on unrelated failures | F design; baseline is historical (53/55), not re-run here |
| A5 | Coverage reporting, report-only, no threshold (F) | S / Low | Untested-code visibility (line/branch deltas per milestone) | Coverage tooling breaks Windows parity or produces misleading numbers on generated code | F design |
| A6 | Native scheduled libFuzzer + corpus management (F) | M / Low | New crash/hang classes found per corpus; corpus growth | No new findings after a bounded campaign, or harness maintenance exceeds value | F design; 6 harnesses already exist in main |
| A7 | ps2sdk reference pinning table (F) | S / Low | Register/ABI facts pinned with citations, replacing "unpinned" notes | A pinned fact contradicts a tested behaviour (then the test wins and the pin is corrected) | F design; reading headers was never actually blocked |
| A8 | Metamorphic relations — recompilation determinism first (F) | S / Low | Same input → byte-identical generated code, checked in CI | Determinism fails for legitimate reasons (timestamps, paths) that cannot be normalised | F design |
| A9 | Hypothesis pilot for Python semantics models (F) | S / Low | Property violations in the Python models (the Z3 hook's neighbours) | No violations found and model coverage already sufficient | F design |
| A10 | Host-optimisation contract as process (B) | S / Low (document + review habit) | Every future "optimisation" arrives with interleaved A/B numbers or not at all; byte-identical checkpoints retained | The contract is written but never enforced in review (then it is decoration — the matrix records this failure mode explicitly) | B experiment E7 |

### 1.2 PROTOTYPE (in priority order — build smallest falsifiable experiment first)

| # | Candidate (category) | Effort / Risk | Benefit metric | Falsifier | Experiment / evidence |
|---|---|---|---|---|---|
| P1 | CCC `stdump` standalone converter (A) | S / Low (MIT, no JVM standalone) | `.mdebug`/STABS symbols recovered where present, with 0 FP | No Zmbor-relevant input carries `.mdebug`, or conversion adds FP | A design; converter NOT run |
| P2 | Confidence-labelled discovery artifact + gap report (B) | S / Low (analyzer-only, zero runtime risk) | Coverage of a real ELF by proof class (proven/guarded/scanned) as a tracked number | Labels do not reflect the proof actually done (a mislabelled hypothesis is worse than no label) | B experiment E1 |
| P3 | Chunked generated output (B) | S / Low (one new output option) | Build time + link time on e2e + ps2autotests sets; must include link time | Chunking does not beat single-file including link time | B experiment E2; related to E's TU partitioning (P6) — coordinate, do not duplicate |
| P4 | PS2Recomp-Fixer diagnostics + tools + regression method (C) | M / Medium (overlap adjudication vs Phases 8–11; Windows-first) | Diagnostic yield + regression-method adoption (hash-frozen reference+candidate, name-matched failures) | Three-way triage (vs `eb8688b` vs Zmbor main) shows no transferable diagnostic value, or hunks conflict irreconcilably | C experiment E1 gates any adoption; diff inspected at metadata level only |
| P5 | DQ8 overlay manager (C) | M / Medium | Overlays load/unload correctly with constructors + registration, on a synthetic fixture | Inventory-gap confirmation fails (Zmbor already has an equivalent) or arena semantics do not fit Zmbor's ELF model | C experiment E3 (confirm the gap first) |
| P6 | TU partitioning + PCH together (E) | M / Medium | Game-scale build time: PCH prototype measured TU 3.31 s vs ~6.1 s, break-even ≈ 16 TUs per PCH rebuild (micro-corpus only) | Synthetic-large-corpus experiment does not win including link time | POC 3 PCH prototype RAN (isolated); full experiment is C-design |
| P7 | Clang toolchain prototype, prebuilt 23.x (E) | M / Low-Medium (second compiler; never build LLVM) | Second-compiler diagnostics, ThinLTO access, `-ftime-trace` bottleneck data | No diagnostic or build-time win over GCC, or Windows parity fails | E design |
| P8 | SIMDe narrow probe (D) | S / Low (MIT, header-only) | An sse2neon coverage hole on ARM closed, with 0 regressions | No hole demonstrated (then stop — not ADOPT) | D experiment E4 |
| P9 | paraLLEl-GS adapter (D) | L / High (needs the raw-GIF seam first; author disclaims bit-exact accuracy) | Differential GS correctness (tolerance-0 flat / bounded-tolerance textured / page-hash gates) + scene-scale capacity | Any E2a/systematic/feedback failure (stops the prototype; no tolerance widening), or the seam cannot be built CPU-only and golden-identical | D experiments E1 (fixture harness first) + E2 (adapter + VERIFY) |
| P10 | Fixer GS-DLL spike, then microVU (C) | L / High (Windows/DX-only GS path; SPU2/VU overlap adjudication) | Differential GS/VU correctness vs CPU rasteriser + interpreter | Windows-only path with no Linux story, or overlap conflicts | C sequence: diagnostics first, GS-DLL second, microVU third |
| P11 | TimeSplitters perf + emitter audit patterns (C) | S-M / Low (patterns only) | Transferable pitfall checklist applied to Zmbor's emitter (64-bit branch tests, vf0-per-guest-context, fast-math hazards, false-success stubs, huge-function flags) | Checklist finds nothing new in Zmbor's emitter | C design; **licence UNKNOWN — hard stop on code reuse until confirmed** |
| P12 | SMC analysis + hash-guarded dispatch, dynamic regions only (B) | M / Medium | Self-modifying regions handled without an EE interpreter; guard materialises *all* observable state incl. COP0/VU | Guard observes stale state, or dynamic-region scope cannot be bounded | B experiment E3 |
| P13 | Register-local promotion, gated (B) | S-M / Medium | Context-traffic reduction, only if promotable registers are ≥5% of context traffic on Zmbor's own code mix | Below 5% (then stop before prototyping) | B experiment E4 (measurement gate first) |
| P14 | Linker-priority patching + strict validation + pre-hooks (B) | S / Low-Medium | Bad configs caught at recompile time (strict mode is the highest-value half) | Strict mode rejects valid configs (then the validator, not the configs, is wrong) | B experiment E5 |
| P15 | First-divergence harness + watchdog (B) | S / Low (diagnostic-only) | First divergence found automatically vs `.expected` traces; hangs detected in the e2e runner (gameplay scope only) | Harness cannot distinguish divergence from legitimate nondeterminism | B experiments E3/E7 reporting |
| P16 | New libFuzzer targets: SIF packet parser, then SPU2, then structure-aware analyzer fuzz (F) | M / Medium | New crash classes per target | Targets find nothing new after a bounded campaign | F design; analyzer fuzz after Phases 12/14 land |
| P17 | TSan job, non-blocking first (F) | M / Medium (Windows parity UNKNOWN) | Threading faults ASan/UBSan cannot see | TSan noise exceeds signal on Zmbor's threading model | F design |
| P18 | Z3 translation validation + CFG checks (F) | M / High (FP encodings excluded until a hardware model is selected) | Translation bugs proven, not just tested | Solver `unknown` on the first real property (then cvc5 reopens — see DEFER) | F design |
| P19 | Mull scoped pilot; RapidCheck pilot on pure invariants; SPU2 model-vs-runtime differential (F) | S-M / Low-Medium | Mutation kill rate; property violations; model/runtime divergences with stated tolerances | Pilots find nothing, or RapidCheck's BSD-2 vs BSD-3 discrepancy cannot be resolved (then drop RapidCheck) | F design |
| P20 | reo diagnostic-recording pattern, narrow (C) | S / Low | Opt-in, capped, session-scoped diagnostics adopted without launcher code | Diagnostic volume or privacy/scope issues | C design |

### 1.3 STUDY (paper/design only, with promotion triggers)

| Candidate (category) | Promotion trigger |
|---|---|
| angr as external oracle (A) | A VEX EE lifter that does not silently drop MMI/COP2, plus a precision fix — neither exists today; re-evaluate only on new evidence |
| spimdisasm differential decoder (A) | A second R5900 decoder claim verified against Zmbor's own decoder on the e2e corpus |
| Capstone decoder probe (A) | Never an analyzer for this target; probe role only, on EE-word coverage |
| N64Recomp / N64ModernRuntime / Zelda64Recomp patterns (B) | A Zmbor problem that matches a pattern's proven shape (see B §2 transfer table) |
| Segment-qualified dispatch (B) | Phase 15 COP0 design lands; read psxrecomp's segment design against it — no parallel build |
| Native VU lifter + interpreter handoff (B) | A VU scheduling scaffold exists; read AC5 vurecomp + ps3recomp handoff docs against Zmbor's VU interpreter + Phase 8 gap |
| reo diagnostics (C, broader); GT4Recomp architecture; ps2-web-recomp GPU-GS design (C) | GS workstream commits to a GPU GS (web-recomp); a diagnostics or architecture decision that needs the reference |
| OpenGOAL as contrast (C) | Never as a model — its value is the automation warning, permanently |
| PCSX2 GS as documented reference (D) | Any GS design decision that needs a second documented implementation |
| Tracy runtime-only, default OFF (E) | A real workload + named hotspot exists; instrumentation is measurement, not optimization |
| Google Benchmark for non-header-only kernels (E) | A kernel worth benchmarking outside the hand harness |
| Compiler-flag parity experiment (E) | A flag question that fpenv can actually decide |
| Fast linkers lld/mold (E) | Link time separated from compile time and shown dominant |
| MSan pilot; CBMC bounded pilot (DMA walk, clamp helpers); PCSX2 differential as manual triage; curated clang-tidy (F) | Pilot scope agreed + triage capacity available; PCSX2 differential never becomes a gate or fix driver |

### 1.4 DEFER (with triggers — revisiting before the trigger is waste)

| Candidate (category) | Trigger to revisit |
|---|---|
| Rizin, radare2, BAP (A) | A fourth-oracle need after Ghidra + CCC + internal discovery are all in place |
| Grouped PS2Recomp ports, sotc-vibe-pc, second GT4 port, Outbreak-decomp (C) | A new framework pattern demonstrated in one of them |
| Threaded CPU rasteriser implementation (D) | The measurement experiment shows CPU-side scene cost dominant |
| Granite standalone (D) | A standalone renderer need independent of paraLLEl-GS |
| enkiTS (D) | A measured job-parallel hot spot |
| Bespoke Vulkan compute backend (D) | paraLLEl-GS adapter fails for reasons a bespoke backend would fix (not for performance fashion) |
| PGO (E) | A real workload + named hotspot; runtime-only first (per-game generated-code PGO is a per-port cost) |
| ThinLTO as an independent item (E) | Inside the Clang prototype, with ThinLTO vs full-LTO numbers |
| AFL++ (F) | libFuzzer corpus plateaus; external-only forever under AGPL-3.0 |
| FuzzTest (F) | The Phase 7 reasons for passing on it no longer hold |
| cvc5 (F) | First Z3 `unknown` on a real property |
| KLEE (F) | A bounded, whole-machine-feasible target identified (none today) |
| LLVM/SSA backend, TinyCC runtime-compiled overlays, RT64-style GS backend, ReXGlue (B) | E8 gate (LLVM); licence + code verification (ReXGlue); otherwise rewrite-scale, toolchain-unproven, or out of scope |

### 1.5 REJECT (structural reasons — do not re-propose in the same form)

| Candidate (category) | Structural reason |
|---|---|
| Binary Ninja / IDA Pro (A) | Closed, paid, headless-gated; incompatible with a GPL-3.0 pipeline |
| All code imports from N64Recomp, N64ModernRuntime, XenonRecomp, DolRecomp (PowerPC parts), psxrecomp (licence bar), Zelda64Recomp (game port) (B) | Wrong ISA, wrong licence, or game port — patterns only, re-implemented |
| Outbreakweb browser port as evidence (C) | No public repo; unverifiable |
| PCSX2 GS as code import (D) | Fork-shim weight, two GS state machines, per-game hack layer Zmbor must not import |
| Unity builds for generated code (E) | e2e single-file output already is unity; more unity worsens documented failure modes |
| ClusterFuzzLite as-is (F) | Docs state Docker as a prerequisite; the no-Docker constraint disqualifies it outright |
| OSS-Fuzz hosted (F) | Eligibility unproven for a young single-maintainer project, Docker-based, disclosure burden |

## 2. Prioritized recommendation (the program's answer, in order)

**Do first (prerequisites — everything else's measurements depend on these):**

0. **Scene fixture + timing harness (D rank 0).** Replayable GIF-stream fixtures + CPU-backend
   timings + golden frames. Without this, every GS performance verdict is speculation.
0. **Ghidra pipeline completion conditions (A).** Load a POC 1 CSV through `loadGhidraMap`;
   port label/materialised-address/data-pointer collection into the headless export and
   re-measure. Until then, A1 is ADOPT-the-pipeline, not a proven end-to-end pass.
0. **Fixer three-way triage (C E1) and DQ8 inventory-gap confirmation (C E3).** Both gate
   their prototypes; neither is optional paperwork.

**Then, in order:**

1. **Analysis oracle:** A1 (Ghidra headless pipeline) + P1 (CCC `stdump` converter) + P2
   (confidence-labelled discovery artifact). These three together turn "what do we actually
   cover, and with what proof?" from an unknown into a tracked number with a second opinion.
2. **Build speed:** A2/A3 (sccache/ccache) + P6 (TU partitioning + PCH) + P7 (Clang
   prototype) + P3 (chunked generated output — coordinate with P6, do not duplicate).
3. **Verification gates:** A4–A10 (ps2autotests gate, coverage report-only, native libFuzzer +
   corpus, ps2sdk pinning table, determinism metamorphic relation, Hypothesis pilot,
   host-optimisation contract) + P16–P19 (new fuzz targets, TSan, Z3 translation validation,
   Mull/RapidCheck/SPU2-differential pilots).
4. **Recompiler patterns:** P12–P15 (SMC + hash-guarded dispatch, gated register-local
   promotion, strict patch validation, first-divergence harness + watchdog).
5. **GS capacity (only after the fixture harness + raw-GIF seam):** P9 (paraLLEl-GS adapter)
   with P10 (Fixer GS-DLL spike, then microVU) as the alternative to compare against —
   not both at once. P8 (SIMDe probe) rides along cheaply at any point.
6. **Overlays + diagnostics:** P5 (DQ8 overlay manager, gap confirmed first) + P20 (reo
   diagnostics pattern).
7. **Everything in STUDY/DEFER stays there until its trigger fires.** In particular: no
   angr promotion without a fixed VEX EE lifter; no PGO/ThinLTO decisions outside the Clang
   prototype; no GPU GS commitment without the fixture harness.

**Explicitly not recommended, even though the user listed them:** angr as an analyzer (STUDY
only — VEX silently drops MMI); PCSX2 GS code import (REJECT); unity builds (REJECT);
Docker-based fuzzing (REJECT); any per-game manual porting pattern as a framework direction
(C's guardrail: track manual-hours-per-new-input and the game-specific-vs-shared fraction).

## 3. Sequencing and integration rules (binding on every prototype)

1. **Feature flags for everything behavioural.** Ghidra oracle input is optional input to the
   existing map loader (never a bypass of validation); TU partitioning is a new output
   option (default stays single-file until it wins including link time); PCH is a build
   option; the GS adapter sits behind the existing `PS2X_ENABLE_PARALLEL_GS`-style flag,
   default OFF; Tracy is runtime-only, default OFF.
2. **No wholesale merges.** Fixer hunks are cherry-picked with overlap adjudication vs
   Phases 8–11; N64/PS1 patterns are re-implemented, never imported; PCSX2 is reference,
   never evidence and never a fix driver.
3. **No reapplication of old patches onto main.** Main is at `a1bb50b`; Phases 10b–14 are
   unpublished. A prototype patch that touches discovery, SPU2 guest registers, TODO_NAMED,
   or indirect-CF code must be diffed against main first and state which unpublished work
   it assumes or conflicts with.
4. **Licence gates are hard stops, not TODOs.** TimeSplitters (UNKNOWN) and ReXGlue
   (unresolved) block code reuse until confirmed; angr's GPL-2.0-only status blocks library
   integration permanently (external oracle only); paraLLEl-GS is a separately built library
   only; Granite's exact pin and Vulkan-Headers/VMA licences must be confirmed from pinned
   checkouts before use; RapidCheck's BSD-2 vs BSD-3 discrepancy must be resolved before
   adoption.
5. **Measurement before optimization, always.** The host-optimisation contract (B A10) is
   the enforcement mechanism: ≥5% gate, interleaved A/B, byte-identical checkpoints,
   faithful path retained — checked in review, not just written down.
6. **False positives are failures.** A discovery or analysis result is scored on precision
   first (promotion must be exact); a recall win with FP is a STUDY result, not an ADOPT
   (this is exactly why angr stays at STUDY and Ghidra's 0-FP result matters).
7. **Windows/Linux parity is a release criterion, not a follow-up.** Every Windows-first
   pattern (Fixer GS-DLL, TimeSplitters, reo's .NET-only pieces) needs a Linux story or
   explicit Windows-only scoping before adoption. TSan/MSan/coverage/hwtest Windows parity
   is UNKNOWN and must be measured, not assumed.
8. **Manual-effort accounting (C's guardrail).** Any adopted pattern reports
   manual-hours-per-new-input and the game-specific-vs-shared fraction, so "adopted a
   pattern" cannot silently become "did a manual port."
