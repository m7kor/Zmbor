# Next-Generation Recompiler Design (Incremental)

Research program deliverable 4 of 4. Companion documents: `../research/OPEN_SOURCE_TECHNOLOGY_LANDSCAPE.md`
(landscape), `../research/INTEGRATION_FEASIBILITY_MATRIX.md` (decisions),
`../research/TECHNOLOGY_BENCHMARKS.md` (measurements). This design proposes only what Zmbor
could actually adopt incrementally, in migration order, with feature flags and stated risks.
It is not a rewrite proposal: the recompiler, analyzer, CFA, merge order, runtime, and CPU
rasteriser all stay, and every change below attaches to an existing seam or adds an optional
input.

Design rule that governs everything below: **a new input may propose, but only Zmbor's own
validation may promote.** Ghidra output, angr output, a CCC symbol list, or a Fixer hunk is
a candidate until it passes executable-section, alignment, range, precedence, and CFG
validation — the same rules that govern internal discovery today. Anything that bypasses
validation is not a design, it is a regression.

## 1. What stays (non-negotiable unless a later experiment overturns it)

- **The pipeline shape:** PS2 ELF → analysis → generated C++ → native compilation → playable
  PC port. No interpreter-first redesign, no JIT, no LLVM/SSA backend (DEFERRED — see §6).
- **Discovery + merge order:** symbols > Ghidra map > config extras > discovered, via
  `addOrMerge()` last, with the Ghidra-map set rule (a loaded map's erasure of auto-named
  extras applies to discovered functions; tables exempt). Phase 12/14 rules are preserved
  verbatim: CFA resolves a table only when every target is accounted for; the emitter runs
  CFA with the full function list; direct-J tail-call splitting stays out of scope until
  separately justified.
- **The fallback scan** (materialised code addresses, clustered/descriptor/consumed pointers)
  as the merged-output backstop. New oracles are compared against it, not assumed superior
  to it — angr's POC 2 result (perfect recall, failing precision) is the cautionary example.
- **The runtime and its failure conventions:** Phase 13 loud failure for unimplemented
  syscalls/stubs stays; a false-success stub is worse than a loud failure (TimeSplitters
  checklist, independently confirmed by Phase 13). `-ffast-math` stays forbidden for
  arithmetic-header code (fpenv negative test).
- **The CPU rasteriser as correctness oracle.** Any GPU GS backend is verified against it
  (tolerance-0 flat, bounded-tolerance textured, page-hash gates); it is not removed when a
  GPU backend lands.
- **Full LTO in Release** (`-O3 -flto=auto -fno-fat-lto-objects -ffp-contract=off
  -msse4.1`); single-file generated output as the default until chunked output wins
  **including link time**.
- **Existing tests, hardware baselines, and reproducibility.** No `.expected` edits to make
  a new tool pass; no test weakening; ps2autotests `.expected` files are inputs, not
  variables. Deterministic test inputs throughout.

## 2. What changes, in migration order

### Phase 0 — Prerequisites (do first; everything else's measurements depend on these)

1. **Scene fixture + timing harness (Category D rank 0).** Replayable GIF-stream fixtures +
   CPU-backend timings + golden frames, in-tree. This is the prerequisite for any GS
   performance claim and for the paraLLEl-GS adapter's VERIFY mode. Acceptance: the harness
   reproduces the gs_sprite golden frame and reports CPU-backend timings deterministically.
2. **Ghidra pipeline completion conditions (Category A).** (a) Load a POC 1 CSV through
   `loadGhidraMap` in a test — the ADOPT is for the pipeline, and this execution is the
   missing link between "format-checked" and "proven". (b) Port the in-tree script's label /
   materialised-address / data-pointer collection into the headless export and re-measure
   (functions-only export can only add candidates on porting, and may add false positives —
   re-score precision before any promotion change).
3. **Fixer three-way triage (C E1) and DQ8 inventory-gap confirmation (C E3).** Both gate
   their prototypes; neither is optional paperwork. The triage diffs Fixer against upstream
   `eb8688b` and against Zmbor main, hunk by hunk (the program inspected it at
   document/metadata level only).

### Phase 1 — Analysis oracle (highest value per risk)

4. **Ghidra headless oracle pipeline (ADOPT, A1).** Integration point: the existing map
   loader (`loadGhidraMap` + `elf_parser` map path) — no new loader, no new format. The
   pipeline is: pinned Ghidra 12.1.4 + OpenJDK 21 + EE extension built from a pinned source
   commit (manual build procedure in POC 1 README; Gradle is not relied upon) → headless
   export (`ExportPS2FunctionsHeadless.java`) → CSV → existing validation → merge order
   unchanged. **Feature flag / option:** oracle input is opt-in per recompile invocation
   (a CLI/config switch, default off until the completion conditions pass, then default
   on for stripped inputs only — symbols still win where present). **What it is not:** a
   replacement for internal discovery, a reachability prover (Ghidra reports `dead_func`
   unreferenced; the reachable set still comes from Zmbor's descent), or an end-exactness
   guarantee (exported ends use body-max+1).
5. **CCC `stdump` standalone converter (PROTOTYPE, P1).** Integration point: same map
   loader, fed by a converter that turns `.mdebug`/STABS data into the map format. No JVM
   needed standalone; MIT. Kill condition: no Zmbor-relevant input carries `.mdebug`, or
   conversion adds FP that validation cannot filter.
6. **Confidence-labelled discovery artifact + gap report (PROTOTYPE, P2).** Integration
   point: analyzer-side JSON/TSV emission, no runtime change. Every discovered function
   carries the proof actually done (proven / guarded / scanned); the gap report names what
   is uncovered and why. This turns "how much of a real ELF do we actually cover?" into a
   tracked number and is the acceptance metric for Phases 1–2 as a whole: the number must
   go up on real inputs without precision going down. **Rule:** a mislabelled hypothesis
   is worse than no label — labels are audited by the same tests that audit discovery.

### Phase 2 — Build speed (measured bottleneck: compiling + LTO-linking generated code)

7. **sccache install + docs, local cache only (ADOPT, A2) / ccache alternative (A3).**
   Integration point: the existing sccache hook (default ON, silent no-op without the
   binary) — install the binary and document it; one configure flag for ccache; never both
   launchers at once. No code change. Acceptance: warm-cache rebuild of `ps2_recomp`
   reproduces the POC 3 proxy (5.9× on that subset) on the adopter's machine.
8. **TU partitioning + PCH together (PROTOTYPE, P6) + chunked generated output (P3).**
   Integration points: a new generator output option (chunked/multi-file output — the
   recompiler's multi-file mode exists but is unexercised by e2e) and a build-system PCH
   option. These two are one experiment, not two: chunking without PCH leaves header
   parsing dominant (~6 s per 51 KB TU even at -O0); PCH without chunking has nothing to
   amortise over (break-even ≈ 16 TUs per PCH rebuild, measured). **Default stays
   single-file until the synthetic-large-corpus experiment wins including link time.**
   Coordinate with Category B's chunked-output experiment (E2) — do not run it twice.
9. **Clang toolchain prototype, prebuilt 23.x (PROTOTYPE, P7).** Integration point: an
   optional toolchain selection for generated code (never build LLVM). It carries three
   decisions at once: second-compiler diagnostics, ThinLTO vs the full-LTO baseline, and
   `-ftime-trace` bottleneck data. PGO and ThinLTO-as-independent-items are decided
   inside this prototype, not before it.

### Phase 3 — Verification gates (make regressions loud and coverage visible)

10. **ps2autotests as a CI gate (ADOPT, A4).** Regression-only failure vs a re-established
    baseline; RSQRT/EFU as an explicit allowlist (they are probe-blocked, not gateable).
    The ladder baseline (53/55 historical) must be re-established deterministically first —
    a gate that flakes on unrelated failures is worse than no gate.
11. **Coverage reporting, report-only, no threshold (ADOPT, A5).** No gaming-prone gate;
    Windows parity must be measured, not assumed.
12. **Native scheduled libFuzzer + corpus management (ADOPT, A6).** The no-Docker answer
    to hosted fuzzing. New targets in order: SIF packet parser, then SPU2, then
    structure-aware analyzer fuzz (after Phases 12/14 land — i.e., now, since they have
    landed unpublished; against main, after main catches up). Corpus management is part of
    the adoption, not an afterthought.
13. **ps2sdk reference pinning table (ADOPT, A7).** A cited table of register/ABI facts
    that replaces "unpinned" notes one by one. Reading headers was never actually blocked;
    the table is documentation with citations, and a pinned fact that contradicts a tested
    behaviour loses to the test (the pin is corrected, not the test weakened).
14. **Metamorphic relations (ADOPT, A8) + Hypothesis pilot (A9) + host-optimisation
    contract (A10).** Determinism (same input → byte-identical generated code) checked in
    CI; property tests for the Python semantics models; the optimisation contract (≥5%
    gate, interleaved A/B, byte-identical checkpoints, faithful path retained) enforced in
    review. These three are cheap, and together they are the enforcement mechanism for §5
    rule 5.
15. **Verification pilots (P16–P19, in order).** TSan job, non-blocking first (Windows
    parity UNKNOWN — measure it); Z3 translation validation + CFG checks (FP encodings
    excluded until a hardware model is selected; cvc5 reopens only on the first Z3
    `unknown`); Mull scoped pilot; RapidCheck pilot on pure invariants (only after its
    BSD-2 vs BSD-3 discrepancy is resolved); SPU2 model-vs-runtime differential with
    stated tolerances. MSan, CBMC (DMA walk, clamp helpers only), PCSX2-differential-as-
    manual-triage, and curated clang-tidy stay at STUDY with pilot scopes agreed first.
    PCSX2 differential is never a gate and never a fix driver.

### Phase 4 — Recompiler patterns (small, diagnostic-first)

16. **SMC analysis + hash-guarded dispatch, dynamic regions only (P12).** The cheapest
    credible answer to self-modifying code that does not require building an EE interpreter
    first. The guard must materialise *all* observable state including COP0/VU, or hooks
    observe stale state. Scoped to dynamic regions; unbounded scope is a reject condition.
17. **Gated register-local promotion (P13).** Measurement gate first: if promotable
    registers are <5% of context traffic on Zmbor's own code mix, stop before prototyping.
    Only then prototype, with resume-aware proof (promotion across a resume label, runtime
    call, or delay slot silently loses guest state and is forbidden).
18. **Linker-priority patching + strict patch validation + pre-hooks (P14).** Builds on
    config Zmbor already has; strict mode is the highest-value half (catches bad configs
    at recompile time). A strict mode that rejects valid configs is a validator bug, not
    a config bug.
19. **First-divergence harness + watchdog (P15).** Diagnostic-only: run recompiled output
    against `.expected` traces, stop at the first divergence; gameplay watchdog in the e2e
    runner, scoped to gameplay (legitimate long loads must not trip it).

### Phase 5 — GS capacity (only after Phase 0 + the raw-GIF seam)

20. **ADR 0001 raw-GIF seam (prerequisite, CPU-only, golden-identical).** The seam is
    design only today. Building it is the first GS step, and its acceptance is
    golden-identical CPU output — if the seam changes a frame, the seam is wrong, not the
    backend. Guest-visible SIGNAL/FINISH/LABEL/CSR stay in the front end; `ReadVram`/
    snapshot through GPU VRAM needs explicit page-tracker syncs.
21. **paraLLEl-GS adapter (PROTOTYPE, P9) vs Fixer GS-DLL spike (P10) — compare, do not
    build both at once.** The adapter is isolated, behind a feature flag (same shape as
    the prior assessment's `PS2X_ENABLE_PARALLEL_GS`, default OFF), readback presentation,
    VERIFY mode with tolerance-0 flat / bounded-tolerance textured / page-hash gates; any
    E2a/systematic/feedback failure stops the prototype with no tolerance widening. The
    DLL spike is the alternative to compare (Windows/DX-only — needs a Linux story).
    Granite is a dependency of the adapter only (its exact pin is unverified — confirm
    from a pinned checkout; HEAD `69f450c` is NOT the pin). The bespoke Vulkan backend is
    DEFERRED: tooling is the easy 5%, and paraLLEl-GS already wrote the other 95%.
    SIMDe's narrow probe (P8) rides along cheaply at any point (stop if no sse2neon hole
    is demonstrated). Threaded CPU rasteriser implementation stays DEFERRED until its
    measurement experiment shows CPU-side scene cost dominant.

### Phase 6 — Overlays, diagnostics, and the project pipeline (last, because they depend on 1–5)

22. **DQ8 overlay manager (PROTOTYPE, P5) — gap confirmed first.** Load/unload arena +
    constructors + function registration, on a synthetic fixture. If the inventory-gap
    confirmation fails (Zmbor already has an equivalent), stop. Overlays are the one
    capability in this program with no counterpart in Zmbor's inventory, which is why they
    sit here rather than earlier: they matter for real games, and real games are not yet
    the binding constraint — recompiler correctness is.
23. **reo diagnostic-recording pattern, narrow (P20).** Opt-in, capped, session-scoped
    diagnostics; no launcher code. This pairs with the Fixer diagnostics (P4) — adopt one
    pattern, not two competing diagnostic systems.
24. **Project-generation pipeline (STUDY → design, from the mission assessment's gap).**
    This program found no candidate that delivers it off the shelf: the port projects'
    pipelines are per-game manual (DQ8's verify→extract→recompile shape is the closest
    reusable pattern), and Studio is a GUI, not a batch generator. The design task is to
    specify a batch generator (ELF in → generated project out, with the confidence-labelled
    gap report from Phase 1 as its input manifest) — specified from Phases 1–5, not built
    before them. The Fixer regression method (hash-frozen reference+candidate,
    name-matched failures) is the testing shape for it.

## 3. Integration points (exact, per change)

| Change | Integration point (file / API, in main) | Flag / option |
|---|---|---|
| Ghidra oracle input | Existing map loader: `loadGhidraMap` + `elf_parser` map path; headless export format = the in-tree script's format, extended | Opt-in CLI/config switch; default off until Phase 0 conditions pass, then on for stripped inputs only |
| CCC converter | Same map loader, via a converter into the map format | Same switch family as above |
| Discovery artifact + gap report | Analyzer-side JSON/TSV emission (new output, no runtime change) | Always emitted in analysis runs; consumed by CI + the project generator |
| sccache / ccache | Existing sccache hook (`ps2xRuntime/CMakeLists.txt`, default ON); ccache via one configure flag | Env/config; never both launchers |
| TU partitioning + PCH | New generator output option (multi-file mode) + build-system PCH option | Output option (default single-file); PCH build option |
| Clang toolchain | Toolchain selection for generated code | Toolchain option; GCC stays default |
| ps2autotests gate / coverage / libFuzzer scheduling | CI configuration (`build.yml` gains a hwtest step; coverage report; scheduled fuzz job) | Gate (regression-only + allowlist); coverage report-only; fuzz job scheduled, not blocking, until proven stable |
| ps2sdk pinning table | Documentation table with citations (no code) | — |
| Determinism / Hypothesis / optimisation contract | CI check + Python test models + review checklist | CI-enforced (determinism); review-enforced (contract) |
| SMC dispatch / promotion / patch validation / divergence harness / watchdog | Emitter + analyzer passes; runtime diagnostics; e2e runner | Emitter options / diagnostic flags, default off where behavioural |
| Raw-GIF seam + GS adapter | `GifArbiter` → `GS::processGIFPacket` path; `GSRasterBackend` interface (second implementation) | Seam: always present once built (CPU-only, golden-identical); adapter behind `PS2X_ENABLE_PARALLEL_GS`-style flag, default OFF |
| Overlay manager | ELF loading + function registration + arena management (new subsystem, gap confirmed first) | Feature-flagged subsystem |
| Diagnostics pattern | Runtime diagnostics (opt-in, capped, session-scoped) | Opt-in flag, capped, session-scoped |

## 4. Feature flags and defaults (summary)

- Oracle input: opt-in → on-for-stripped-only after completion conditions.
- TU partitioning / chunked output: new option, default single-file until it wins including
  link time.
- PCH: build option, off by default until the large-corpus experiment wins.
- Clang: toolchain option, GCC default.
- GS adapter: flag, default OFF, VERIFY mode available regardless of the flag.
- Tracy: runtime-only, default OFF.
- Fuzz/TSan jobs: scheduled / non-blocking until proven stable; ps2autotests gate is
  blocking only for regressions vs the re-established baseline (allowlist for RSQRT/EFU).
- Diagnostics: opt-in, capped, session-scoped.

## 5. Risks and mitigations

| Risk | Mitigation in this design |
|---|---|
| A new oracle's FP poisons the function set | Promotion only through Zmbor's validation; precision scored first; Ghidra's 0-FP result is the bar, angr's extras are the warning |
| A mislabelled discovery artifact misleads the project generator | Labels audited by discovery tests; a mislabelled hypothesis is treated as a bug, not a cosmetic issue |
| GS adapter diverges subtly from the CPU rasteriser | VERIFY mode with tolerance-0 flat / bounded-tolerance textured / page-hash gates; any systematic failure stops the prototype; CPU rasteriser stays as oracle |
| Build-system changes break Windows parity | Windows parity is a release criterion; TSan/MSan/coverage/hwtest parity measured, not assumed; MSVC legs (`musttail`, `/fp:fast`) compiler-gated |
| Fixer hunks conflict with Phases 8–11 or import PCSX2 behaviour as fact | Cherry-pick with overlap adjudication; PCSX2 is reference, never evidence; three-way triage gates adoption |
| Licence traps (angr GPL-2.0-only, TimeSplitters UNKNOWN, ReXGlue unresolved, Granite pin, VMA/Vulkan-Headers provisional, RapidCheck BSD discrepancy) | Hard stops in the matrix §3; external-oracle-only or separately-built-library shapes; confirm from pinned checkouts before use |
| "Adopted a pattern" silently becomes "did a manual port" | Manual-hours-per-new-input + game-specific-vs-shared fraction tracked for every adopted pattern |
| A prototype that fails is kept alive as "phase 2" | Prototype verdicts are binary: pass the falsifier or become REJECT; STUDY/DEFER triggers are the only route back |
| Upstream drift (ran-j/PS2Recomp unfetchable; EE extension HEAD moves) | Pins are commits, not "latest"; the EE extension pin is a source commit with a manual build procedure, not a Gradle dependency |

## 6. What is deliberately NOT proposed (and why)

- **No recompiler rewrite, no LLVM/SSA backend, no new IR.** The SSA benefit evaporates at
  resume labels (every resume label becomes an exit boundary); the LLVM gate excludes some
  hosts; the rewrite cost dwarfs every measured benefit in this program.
- **No angr as an analyzer, no Capstone as an analyzer, no Rizin as an analyzer.** VEX
  silently drops MMI; Capstone misdecodes 5.7% of vu_macro `.text` words on EE instructions;
  Rizin is generic MIPS, unproven here. Oracles and probes only.
- **No PCSX2 code import (GS or otherwise).** Fork-shim weight, two state machines, and a
  per-game hack layer Zmbor must not import. PCSX2 stays a documented reference.
- **No Docker-based fuzzing, no OSS-Fuzz hosted, no ClusterFuzzLite as-is.** The no-Docker
  constraint is structural; native scheduled libFuzzer is the answer.
- **No unity builds, no `-ffast-math` in arithmetic-header code, no "add LTO".** Each is
  already settled by existing evidence (single-file output already is unity; fpenv forbids
  the flag; full LTO is already on).
- **No per-game manual porting as a framework direction.** The port projects' pipelines
  are per-game manual by construction; Zmbor's mission is automation, and the
  project-generation pipeline (§2 Phase 6) is specified from the automated pieces, not from
  a port's manual workflow.
- **No audio sink, no SIF2, no DMA-timing work in this design.** They are below the top 5
  blockers by the mission assessment and are not advanced by any candidate evaluated here;
  the audio sink in particular stays gated on a real title running graphically with silence
  as the remaining defect.
- **No commercial-game claim, no real-game end-to-end claim, no hardware claim.** Nothing
  in this program ran a game, touched a physical PS2, or proved hardware behaviour. The
  design is for the pipeline; games are its future validation, not its present evidence.

## 7. Open questions (owned, not hidden)

1. Does Zmbor already have an overlay equivalent? (C E3 must confirm the gap before P5 is
   built — the design assumes it does not, on Category C's evidence.)
2. What is the exact Granite pin that paraLLEl-GS `cc6184a` expects? (Unverified — submodule
   empty in shallow clone. Confirm from a pinned checkout before any adapter work.)
3. What are the Vulkan-Headers/VMA licences from pinned checkouts? (Provisional only today.)
4. What is the TimeSplitters licence? (UNKNOWN — hard stop on code reuse until confirmed.)
5. Can upstream ran-j/PS2Recomp be fetched by another route, or is it genuinely
   access-restricted? (Auth prompt on anonymous fetch — recorded UNKNOWN, not circumvented.)
6. What does the label-collection port do to Ghidra's precision? (Re-measure after porting;
   the current 0-FP result is for the functions-only export.)
7. What is Zmbor's actual scene-scale GS cost? (Unknown until the Phase 0 fixture harness
   exists — every GS number today is either micro-corpus wall-clock or someone else's.)
8. What is the Windows parity story for TSan/MSan/coverage/hwtest? (UNKNOWN — measure it
   as part of Phase 3, do not assume it.)
