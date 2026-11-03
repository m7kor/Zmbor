# Zmbor Open-Source Technology Landscape

Research program deliverable 1 of 4. Companion documents: `INTEGRATION_FEASIBILITY_MATRIX.md`
(prioritized recommendations), `TECHNOLOGY_BENCHMARKS.md` (all measurements with commands),
`../architecture/NEXT_GENERATION_RECOMPILER_DESIGN.md` (incremental adoption design).
Full per-candidate template entries live in the research section files under
`~/workspace/zmbor-research/sections/` (category-a … category-f); this document carries a
condensed entry for **every** candidate so the landscape is complete in one place, plus the
Stage 1 inventory that grounds every verdict. Where this document and a section file disagree,
the section file's evidence wins and the discrepancy should be reported.

Status vocabulary used throughout: IMPLEMENTED / TESTED / VERIFIED / HARDWARE-CONFIRMED /
UNVERIFIED / UNKNOWN. Nothing in this program is HARDWARE-CONFIRMED: no physical PS2 was
available, no commercial game was run, and no commercial-game compatibility is claimed anywhere.

## 0. Stage 1 — Synchronize and inventory

### 0.1 What "main" contains vs what is unpublished-only

Published main was cloned anonymously on 2026-11-03 into the research scratch area:

- Main HEAD: `a1bb50be3c797025d96e5c744e615c632b0a8f83`, dated 2026-11-03 14:08:55 +0300.
- Unpublished baseline: `350ca1f3b2cd2aede0d5ec8491b36d2c8a552141` (2026-10-28).
- The unpublished working tree (read-only for this program; a Phase 15 COP0 coordinator works
  there) is 50 files vs baseline: 48 Phase-14 files plus COP0 work in progress.

Verified by grep against main (not assumed from the phase history):

| Work | In main? | Evidence |
|---|---|---|
| Phase 3 clamp fixes, Phase 4 SIF0, Phase 5 test infra | Yes | `sif0_fifo.h` present; root `enable_testing()` |
| Phase 7 pitch / Gaussian, Phase 8 VU accuracy | Yes | `kGaussTable`, `vf0Scratch` present |
| Phase 9 mixing, Phase 10 reverb, Phase 11 ADMA | Yes | `mixTick`, `stepReverbCore`, `setAdmaEnabled` present |
| Pre-Phase-13 TODO_NAMED (throws calls 1–8, then suppresses) | Yes | Old suppression message present; "still fails loudly" absent |
| Phase 10b SPU2 guest-register integration | **No** | `reverbParams` = 0, `guestGate` = 0 in main |
| Phase 12 stripped-ELF discovery | **No** | `discoverFromEntryPoint` = 0 in main |
| Phase 13 TODO_NAMED loud failure | **No** | See TODO_NAMED row above |
| Phase 14 indirect-CF v2 | **No** | `scanDataPointers` = 0 in main |
| Stripped e2e program | **No** | `program_stripped` = 0 in main's `e2e_programs.py` |

Consequence, stated plainly: **every "integration" verdict in this program is stated against
main, not against the unpublished tree.** A candidate that duplicates Phase 12/14 discovery, or
Phase 10b guest registers, looks redundant against the working tree but is still absent from
main — and Gabriel integrates the unpublished phases himself, so the program must not reapply
old cumulative patches onto main. Where a candidate overlaps unpublished work (notably the
PS2Recomp-Fixer SPU2/VU hunks vs Phases 8–11, and any discovery tool vs Phases 12/14), the
matrix records overlap adjudication as a gating step, not an afterthought.

### 0.2 What Zmbor already has (main)

- **Pipeline:** PS2 ELF → analysis → generated C++ → native compilation → playable PC port.
  The analyzer builds functions from symbols + Ghidra map + config extras + a fallback scan
  (which already proposes materialised code addresses and clustered/data-pointer tables — any
  analysis tool must beat *that*, not a straw man).
- **Ghidra touchpoints already in main (POC 1 is not greenfield):**
  `ps2xRecomp/tools/ghidra/ExportPS2Functions.java`, Ghidra-map loading in `elf_parser.cpp` /
  `config_manager.cpp`, map use in `ps2xAnalyzer`, and `ps2xAnalyzer/Readme.md`. The existing
  export is a GUI-plugin script; the headless export in POC 1 is the new piece.
- **Runtime:** `ps2xRuntime` provides kernel/syscall stubs (Phase 13 loud failure is
  unpublished-only; main still has the capped TODO_NAMED), memory with a TLB table
  (`translateAddress`/`tlbRead`/`tlbWrite`/`tlbProbe` exist — COP0 Phase 15 is auditing what
  they actually cover), VU/GS/SPU2/IOP/DMAC/SIF models. GS: CPU rasteriser only
  (`GSRasterBackend`, single implementation `GSCpuBackend`); `gs.vulkan`/`gs.pgs` MISSING.
  VU execution is synchronous (VBS/D/T interrupts not delivered).
- **Dependencies (main CMake FetchContent; confirm GIT_TAGs in the CMakeLists files):**
  ELFIO, toml11, fmt, libdwarf, rabbitizer, nlohmann/json, raylib, imgui (+ rlImGui,
  ImGuiColorTextEdit, imgui_club, ImGuiFileDialog), SDL, sse2neon. Known cache versions:
  elfio Release_3.12, toml11 v4.4.0, fmt 12.1.0, nlohmann_json v3.11.3, libdwarf v2.2.0,
  rabbitizer 1.14.3, raylib 5.5. Zmbor is GPL-3.0.
- **Test / verification infrastructure (main):** `ps2x_tests`, CTest (including
  `z3_int_semantics`, skipped with code 77 when the z3 solver is absent), Python-assembled
  e2e micro-programs (basic, branchdelay, vu_macro, gs_sprite — exact expected outputs),
  sanitizer build modes, `tools/bench/`, `tools/semantics/`, and docs
  (`docs/architecture/benchmark-methodology.md`, `fuzzing.md`, `hardware-probes.md`,
  `phase7-tool-evaluation.md`, `indirect-call-entry-analysis.md`). The IOP standalone suite
  and the ps2autotests ladder (historical 53/55, 8202/8250 lines; RSQRT/EFU known failures)
  are **not** wired as gates in main. Main also has `docs/INTEGRATION_MATRIX.md` (an earlier
  matrix) and `BENCHMARKS.md` / `TESTING.md` at the root — this program's documents are new
  work, not replacements for them, and do not overwrite them.
- **Known gaps (mission assessment, still the ranking backbone):** (1) stripped-ELF discovery
  (partly addressed unpublished by Phase 12); (2) indirect control flow (partly addressed
  unpublished by Phases 12/14); (3) silent syscall surface (addressed unpublished by Phase 13);
  (4) GS has no performant backend; (5) COP0 TLB/cache/exceptions (Phase 15 in progress).
  Below those: synchronous VU, unmeasured generated-code performance, no automated
  game-to-C++ project pipeline (Studio is a GUI, not a batch generator), SIF1/SIF2/DMA-timing
  gaps, RSQRT/EFU probe-blocked last bits. The audio sink is below the top 5 by the
  assessment's verdict and is not a candidate in this program.

### 0.3 Cross-cutting constraints applied to every candidate

- No Docker anywhere (this disqualifies, not reshapes, ClusterFuzzLite as-is — see §6).
- Native Windows + Linux compatibility preferred; Windows-only patterns need a Linux story or
  explicit Windows-only scoping before adoption.
- GPL-3.0 compatibility is judged per candidate including transitive dependencies; a
  GPL-2.0-only tool (angr's trap) and an unresolved licence (TimeSplitters, ReXGlue) are
  hard stops on code reuse even when design findings remain usable.
- No wholesale replacement of working code without measurable benefit; no rewrite of the
  recompiler; experimental integrations sit behind feature flags; deterministic test inputs;
  false positives are reported as seriously as missed functions; code-generation correctness
  is separated from performance.
- R5900 ≠ generic MIPS. The EE has MMI instructions, COP2/VU, 128-bit registers and
  load/store-quadword semantics that generic MIPS tooling misdecodes or silently drops.
  Any tool claim below that rests on generic MIPS support is marked UNVERIFIED for R5900.

## 1. Category A — Binary analysis and function recovery

Section file: `sections/category-a-binary-analysis.md`. POC 1 and POC 2 both RAN (see
`TECHNOLOGY_BENCHMARKS.md`); this section records the verdicts, not just the research.

| Candidate | Version/commit examined | Licence + GPL-3.0 verdict | R5900 capability (verified) | Zmbor deficiency addressed | Method | Verdict |
|---|---|---|---|---|---|---|
| Ghidra + Headless Analyzer + Emotion Engine Reloaded | Ghidra 12.1.4; EE extension built from source HEAD `4c787f8` (v2.1.39+head) | Ghidra Apache-2.0, EE extension licence as inspected in its repo — compatible as an external tool; no Ghidra code is vendored into Zmbor | EE processor module verified by running it: exact boundaries on all 5 fixtures incl. stripped, 0 FP | Stripped-ELF function recovery as a second opinion to internal discovery; boundary evidence for the existing map loader | External CLI tool (headless export → CSV → existing `loadGhidraMap` validation) | **ADOPT (headless pipeline, with completion condition)** |
| CCC `stdump` (standalone path) | As inspected in the CCC repo (see section file) | MIT — compatible | `.mdebug`/STABS reader only; not a general analyzer | Symbol/debug-info recovery where `.mdebug` exists | External CLI tool / converter prototype | **PROTOTYPE** |
| angr (CFGFast, VSA, symbolic execution) | 10.0.5, CFGFast, MIPS32 LE auto-detected | GPL-2.0-only trap for library integration — external-oracle use only; licence blocks vendoring | **Measured damage:** COP2/LQ lift as `NoDecode`, SQ as `SigILL`, MMI as a silent no-op (bare IMark, no data flow). Perfect recall, failing precision (4 FP on synthetic) | Indirect-CF oracle in theory; in practice does not beat the fallback scan where it matters (promotion must be exact) | External oracle only, never an analyzer of record | **STUDY** |
| spimdisasm | As inspected (see section file) | Licence per section file | Only other R5900 decoder claim found | Differential-decoder role only | Reference / differential probe | **STUDY** |
| Capstone | 5.0.9 (probed) | BSD — compatible as a library | **Measured misdecode:** LQ→`xori.b`, SQ→`ext`/no-decode, MMI→`madd`/no-decode, COP2→no-decode; 13/227 vu_macro `.text` words fail (5.7%), clustered on EE instructions | Decoder probe only | Library, probe only | **STUDY (decoder probe only)** |
| Rizin | As inspected | Licence-clean | Generic MIPS, unproven on R5900 here | Fourth-oracle fallback | External tool | **DEFER** |
| radare2 / BAP | As inspected | Per section file | Lineage duplicate (radare2) / unevidenced (BAP) | None demonstrated | — | **DEFER** |
| Binary Ninja / IDA Pro | Current commercial releases (see section file) | Closed, paid, headless-gated | Not evaluated on R5900 (licence-gated) | None — incompatible with a GPL-3.0 pipeline | — | **REJECT** |

A's completion conditions (not optional extras): (1) port the in-tree script's label /
materialised-address / data-pointer collection into the headless export and re-measure —
the current export is functions-only; (2) load a POC 1 CSV through `loadGhidraMap`
(format-checked only in this program; no Zmbor build was run in Category A); (3) prototype
the CCC standalone converter (needs no JVM). Ghidra's list is a boundary list, not a
proven-reachable set (`dead_func` is reported unreferenced); Ghidra's exported ends use
body-max+1, so end-exactness is NOT demonstrated (starts, the scored quantity, are
unaffected). The synthetic fixture is small and MIPS-subset-only (no MMI, no COP2, no
relocations, no overlays, no C++/vtables, no PIC, no real-game shapes) — passing it is
necessary, not sufficient. The Phase 7 judgement ("no automated benefit") is overturned
**for the headless case only**, on measured evidence.

## 2. Category B — Static recompilation engineering

Section file: `sections/category-b-static-recompilation.md`. Nothing in this category was run;
all verdicts below are pattern-transfer judgements, and the section file's experiment
designs (E1–E8) are the deciding experiments. One rule governs the whole category:
**patterns: study/prototype as re-implementation; code: no imports.**

| Candidate | Verdict | Transferable pattern (re-implement, never import) | What does NOT transfer |
|---|---|---|---|
| N64Recomp (MIPS III → C) | **STUDY (patterns) / REJECT (source)** | Function dispatch shapes, patch/config discipline | N64 CPU/ABI/memory model; RCP-specific runtime |
| N64ModernRuntime (librecomp + ultramodern) | **STUDY** | Runtime-boundary separation (what the runtime owns vs generated code) | N64 threading/rendering assumptions; host scheduling that breaks deterministic e2e comparison |
| Zelda64Recomp (complete N64 port) | **STUDY** | End-to-end port discipline, config + patch workflow | Per-game manual effort; N64-specific everything |
| XenonRecomp (PowerPC → C++) | **PROTOTYPE (2 patterns only: register-local promotion analysis, weak-alias hooks) / REJECT (source, dispatch)** | Register-local promotion with resume-aware proof (gated on measurement: stop if promotable registers are <5% of context traffic); weak-alias hooks | PowerPC ISA; ABI assumptions invalid on PS2 (promotion across a resume label, runtime call, or delay slot silently loses guest state) |
| psxrecomp (PS1 MIPS R3000A → C) | **STUDY** (licence bar on source) | Confidence-labelled discovery artifact + gap report (PROTOTYPE #1 below); host-optimisation contract as process (ADOPT below); source-invariant guards; first-divergence harness shape (oracle = ps2autotests `.expected`, never PCSX2) | PS1 CPU (no EE/VU); its source cannot be reused under the licence bar |
| DolRecomp (GameCube/Wii PowerPC → C or LLVM) | **PROTOTYPE (SMC analysis + hash-guarded dispatch + interception-guard hook design) / DEFER (LLVM backend)** | SMC analysis + hash-guarded dispatch scoped to dynamic regions only; guard must materialise *all* observable state incl. COP0/VU or hooks observe stale state | SSA/IR backend: every resume label becomes an exit boundary, erasing the SSA benefit; LLVM 19/20 gate excludes some hosts |
| PS2Recomp upstream (ran-j, Zmbor's own ancestor) | **STUDY (as a diff baseline, on a schedule)** | Diff baseline for what Zmbor changed vs its ancestor | Upstream could not be fetched anonymously on 2026-11-03 (auth prompt) — status UNKNOWN, not dead; recorded, not circumvented (Category C found the same) |
| Ace Combat 5 static recompilation (sal063, independent PS2 recompiler) | **PROTOTYPE (chunked output, musttail probe, watchdog) / STUDY (native VU1 dual path)** | Chunked generated output (43-file pattern); guaranteed tail calls `[[gnu::musttail]]` compiler-gated (MSVC has no equivalent); gameplay watchdog + capture replay (scoped to gameplay, not loads) | Its VU1 dual path until a VU scheduling scaffold exists |
| ps3recomp (Cell PPU+SPU lifters) | **STUDY** | Interpreter-fallback handoff protocol shape; per-game scaffold kept out of the library (thin, versioned) | Cell/SPU architecture |
| RT64 (N64 graphics renderer) | **STUDY (interface idea only; GS workstream)** | Deferred-log renderer / faithful-vs-enhanced dual-path *interface idea* | A replay-only renderer that does not synchronise game-visible GS memory corrupts gameplay state — no code |
| ReXGlue SDK (Xbox 360 recompiler + runtime) | **DEFER (licence + code verification)** | Nothing until licence and code are verified | Web-only evidence |

B's ranked prototypes (by expected value ÷ (effort × risk)): (1) confidence-labelled
discovery artifact + gap report — analyzer-only, zero runtime risk, turns "how much of a
real ELF do we actually cover, and with what proof?" into a tracked number; (2) chunked
generated output — one new output option, measurable against e2e + ps2autotests sets;
(3) SMC analysis + hash-guarded dispatch, dynamic regions only; (4) register-local
promotion, gated on measurement; (5) linker-priority patching + strict patch validation +
pre-hooks (strict mode is the highest-value half: catches bad configs at recompile time);
(6) segment-qualified dispatch — STUDY coordinated with the Phase 15 COP0 design, no
parallel build; (7) native VU lifter + interpreter handoff — STUDY until a VU scheduling
scaffold exists; (8) **ADOPT (process, not code): host-optimisation contract** — ≥5% gate,
interleaved A/B, byte-identical checkpoints, faithful path retained, checked in review;
(9) first-divergence harness + watchdog, diagnostic-only. DEFER: LLVM/SSA backend,
runtime-compiled overlays via TinyCC, RT64-style GS backend, ReXGlue.

## 3. Category C — Existing PS2 native-port projects

Section file: `sections/category-c-ps2-native-ports.md` (7 anonymous shallow clones; no
game-specific functions, addresses, tables, or hacks reproduced anywhere). Framing finding
that reorders this whole category: **Zmbor is itself a fork of ran-j/PS2Recomp**, so most
"ports" here are sibling forks of Zmbor's own ancestor, not alternative technologies — the
differences are runtime backends, per-game config, and diagnostic/test discipline. No game
was run; all playability/fps claims below are project- or aggregator-reported (UNVERIFIED).
Single-game evidence dominates (Fixer ~2 games, the rest one each).

| Candidate | Version/commit examined | Licence + GPL-3.0 verdict | Reusable pattern (general-purpose only) | What is game-specific / NOT reusable | Verdict |
|---|---|---|---|---|---|
| PS2Recomp-Fixer (`w1284603830/PS2Recomp-Fixer`) | HEAD `05807db`, 2026-11-03, VERIFIED by clone | GPL-3.0 — compatible | PCSX2 hardware GS as a DLL; microVU for VU1; threaded IOP/SPU2/SIO2/IPU; ~150 fixes as an ordered, cherry-pickable patch series against upstream `eb8688b` (the base Zmbor's CHANGELOG cites); opt-in `PS2_*` diagnostics; function-map repair tooling; regression method (hash-frozen reference+candidate, name-matched failures) | Windows/DX-only GS path; every SPU2/VU hunk needs overlap adjudication vs Zmbor Phases 8–11; the diff was inspected at document/metadata level, not hunk-by-hunk | **PROTOTYPE** — diagnostics + tools + regression method first, GS-DLL spike second, microVU third; gated by a three-way triage (vs `eb8688b` vs Zmbor main). No wholesale merge |
| TimeSplitters perf + emitter audit (`GTTeancum/Timesplitters`) | `94079de` | **Licence UNKNOWN — hard stop on code reuse until confirmed** (design findings unaffected) | Best-measured methodology in the category (defined metric, fixed-seed repeatable route, staged 5→60 fps table); VU1 AOT with interpreter fallback + flag liveness; threaded rasteriser; pitfall checklist: 64-bit branch tests, vf0-per-guest-context, fast-math flag hazards, false-success stubs (worse than loud failure — consistent with Phase 13), huge-function optimisation flag | Its game-specific code | **PROTOTYPE (patterns + audit only, licence-gated)** |
| DQ8-Recompiled (`Sinan-Karakaya/DQ8-Recompiled`) | `f6dfd96` | GPL-3.0 — compatible | Overlay manager (load/unload arena + constructors + function registration) — the only capability here with no counterpart in Zmbor's Stage 1 inventory (flagged as an inventory gap to confirm, experiment E3); verify→extract→recompile pipeline shape; GS dump/replay oracle pattern | Its bespoke GS backend | **PROTOTYPE** (overlay manager + pipeline shape; not the GS backend) |
| reo — Resident Evil Outbreak recompilation (`Ninjacornix/reo`) | `7542cc2` | GPL-3.0-only | Diagnostic-recording pattern (opt-in, capped, session-scoped); hostile-path pipeline tests | Launcher code; openSNAP co-op (game-specific) | **STUDY → narrow PROTOTYPE** (diagnostics pattern only) |
| GT4Recomp (`moesuito/gt4-staticrecomp`) | `795cdc0` | MIT own code | Best architecture/process reference: honest hybrid-recomp framing; VU AOT verified against an interpreter; GS as a separate process over a versioned stream protocol (the clean alternative to Fixer's DLL — compare in experiment E8); fixture-per-module tests; task/slice workflow | No runtime import (different lineage) | **STUDY** |
| ps2-web-recomp (`snuri00/ps2-web-recomp`) | `e026b97` | MIT | Best GPU-GS design reference: GS memory resident on GPU, dispatch batching (650→80/frame), 16 MB upload cache (~60% less per-frame data), software rasteriser kept as correctness oracle | Web target specifics | **STUDY** (revisit only if Zmbor commits to a GPU GS) |
| OpenGOAL | As inspected | ISC, VERIFIED licence | Warning value only: decompilation, not recompilation; per-game manual effort at OpenGOAL scale fails Zmbor's automation mission by definition | Everything game-specific | **STUDY (contrast only)** |
| Grouped PS2Recomp ports (Urban Reign, Def Jam Vendetta, Burnout Dominator, Killzone, Burnout 3, God of War, War of Monsters, Ace Combat 5); sotc-vibe-pc; second GT4 port; Outbreak-decomp | Aggregator-verified only | Per section file | No new framework pattern found | Per-game work | **DEFER** |
| Outbreakweb browser port | No public repo | — | Nothing verifiable | — | **REJECT (as evidence)** |

Cross-cutting guardrail proposed by Category C and adopted by this program: track
manual-hours-per-new-input and the game-specific-vs-shared fraction of any adopted work, so
"adopted a pattern" does not silently become "did a manual port." Every Windows-first
pattern here needs a Linux story or explicit Windows-only scoping before adoption.

## 4. Category D — Graphics and runtime

Section file: `sections/category-d-graphics-runtime.md`. Nothing was built or run in this
category (no Vulkan GPU assumed); BT3's paraLLEl-GS numbers quoted below are BT3's, not
Zmbor's, and do not transfer. The headline finding governs the whole category: **there is
no measured scene-at-speed GS bottleneck in Zmbor today, so no candidate earns ADOPT on
performance grounds.** `gs_sprite` is dominated by its guest wait loop plus host start-up
(see `TECHNOLOGY_BENCHMARKS.md` for the conflicting wall-clock figures and their
provenance); its GS work is two flat sprites. The measured VU bottleneck (~50× slower than
real VU1, scheduler bookkeeping) is not a GS problem. The GS case is therefore future
scene-scale capacity + an independent second renderer for differential testing — not current
frame time. GS backend work is blocker 4, below recompiler correctness.

| Candidate | Version/commit examined | Licence + GPL-3.0 verdict | Capability (verified) | Zmbor deficiency addressed | Method | Verdict |
|---|---|---|---|---|---|---|
| Scene fixture + timing harness (prerequisite, not a library) | — | — | Replayable GIF-stream fixtures + CPU-backend timings + golden frames | Without this, every GS performance verdict is speculation | In-tree harness | **DO FIRST** |
| SIMDe (SIMD Everywhere) | `65c15e6` (in-tree 0.8.4), anonymous shallow clone | MIT, header-only, no dependencies — compatible | Portable SIMD header set | Possible sse2neon coverage holes on ARM (no hole demonstrated yet) | Library, option-gated | **PROTOTYPE (narrow)** — test for a hole; not ADOPT without one |
| paraLLEl-GS | `Arntzen-Software/parallel-gs` at `cc6184af7e0c03da603045ca371ffa5dae9b0655`, anonymous shallow clone | LGPL-3.0-or-later — compatible with GPL-3.0 Zmbor **only as a separately built library**, never vendored | Vulkan compute GS, 2×–16× SSAA, VRAM-exact model, `gif_transfer`/`vsync`/`write_register` API; author disclaims hardware bit-exact accuracy | Future scene-scale GS capacity + second renderer for differential testing | Isolated adapter behind a feature flag, readback presentation, VERIFY mode — **only after the ADR 0001 raw-GIF seam lands, CPU-only and golden-identical** | **PROTOTYPE (gated)** |
| Granite (paraLLEl-GS's renderer dependency) | HEAD `69f450c` inspected; the exact Granite pin `cc6184a` expects was **NOT verified** (submodule empty in shallow clone) | MIT (as inspected) | Renderer library | Only as paraLLEl-GS's dependency | Dependency of the adapter | **PROTOTYPE only as dependency; DEFER standalone** |
| Threaded CPU rasteriser + presentation mailbox (no new library) | — | — | Parallelise the existing rasteriser; mailbox decouples presentation | Hypothetical CPU-side scene cost (unmeasured) | In-tree change, measured first | **DEFER implementation; run the measurement experiment** |
| PCSX2 GS (GSdx / GPU plugin) | `6a33137` (sparse anonymous clone) | GPL-3.0+ — licence-compatible, **rejected on architecture** | Documented renderer design (shaders, targets, texture cache) | Reference design only | Reference only | **STUDY as reference; REJECT as code import** (fork-shim weight, two GS state machines, per-game hack layer Zmbor must not import) |
| enkiTS job system | `c947c2c`, anonymous shallow clone | zlib — compatible | Tiny portable job system | A job-parallel hot spot, if one is ever measured (today: single-executor scheduler + one present thread by design — no such workload) | Library, if ever needed | **DEFER** |
| Bespoke Vulkan compute backend (Vulkan-Headers + volk + VMA) | volk `aa76c86` (MIT, cloned); Vulkan-Headers/VMA licences **provisional only** (VMA HEAD via ls-remote only, not cloned — confirm from a pinned checkout before use) | Provisional | Tooling for a from-scratch GS backend | Same future capacity as paraLLEl-GS, but re-implemented | New backend | **DEFER** — tooling is the easy 5%; the GS logic is the other 95% and paraLLEl-GS already wrote it |

Naming trap found (recorded so nobody repeats it): `Themaister/parallel-gs` is **not**
anonymously cloneable (prompts for a username); DW5XL instructions using that path will
fail. The correct public repo is `Arntzen-Software/parallel-gs`. Verified Zmbor integration
points in main `a1bb50b`: `GifArbiter` → `GS::processGIFPacket` (PATH3 via `PS2Memory`,
PATH1 via VU1 XGKICK, PATH2 via VIF1 DIRECT), `GSRasterBackend` interface (only
`GSCpuBackend` exists). The ADR 0001 raw-GIF seam is **design only, not implemented** — the
prototype must build that seam first. Guest-visible SIGNAL/FINISH/LABEL/CSR must stay in the
front end; `ReadVram`/snapshot through GPU VRAM needs explicit page-tracker syncs.

## 5. Category E — Code generation and performance

Section file: `sections/category-e-codegen-performance.md`. POC 3 RAN (see
`TECHNOLOGY_BENCHMARKS.md`); verdicts below are grounded in those measurements, and every
vendor-typical speedup is labelled as such, never as Zmbor's expected speedup. One rule:
**no runtime optimization is recommended before profiling names a hotspot — none has ever
been measured.** Generation is negligible (0.036–0.101 s per e2e program); compiling +
LTO-linking generated code is where time goes.

| Candidate | Version/commit examined | Licence + GPL-3.0 verdict | Capability | Zmbor deficiency addressed | Method | Verdict |
|---|---|---|---|---|---|---|
| sccache | Current release (see section file); hook already exists in `ps2xRuntime/CMakeLists.txt`, default ON, silent no-op without the binary | Apache-2.0/MIT — compatible | Distributed/local compilation cache | 874.8 s full build; 53.7 s incremental TU rebuild | Install binary + docs; local cache only; zero code | **ADOPT** |
| ccache | 4.9.1, installed via apt for the experiment, **measured** 5.9× on the `ps2_recomp` subset (cold 134.0 s vs warm 22.8 s, 117 compilations) | GPL-3.0+ — compatible | Local compilation cache | Same as sccache | One configure flag; never both launchers at once | **ADOPT (measured alternative)** |
| Translation-unit partitioning + precompiled headers (together) | POC 3 PCH prototype (isolated, no clone changes): TU with PCH 3.31 s vs ~6.1 s without; break-even ≈ 16 TUs per PCH rebuild; identical object size | — (build-system change) | Structural fix for game-scale build time | A 51 KB generated TU costs ~6 s even at -O0 (header parsing, not -O level, dominates); e2e uses single-file output while the recompiler's multi-file mode is unexercised | New output option + build option; default stays single-file until the synthetic-large-corpus experiment wins **including link time** | **PROTOTYPE** |
| Clang toolchain (prebuilt 23.x; never build LLVM) | Documented, not run on Zmbor here | Apache-2.0 with LLVM exceptions — compatible as a toolchain | Second compiler for generated code; ThinLTO access; `-ftime-trace` | Compiler monoculture (GCC-only); ThinLTO evaluation; trace-based bottleneck analysis | Optional toolchain | **PROTOTYPE** |
| Tracy Profiler | Documented, not run on Zmbor here; evaluated and deliberately not adopted in-tree (`benchmark-methodology.md`, `phase7-tool-evaluation.md`) | BSD-3 — compatible | Runtime instrumentation / profiling | No named hotspot exists yet — instrumentation is measurement, not optimization | Runtime-only, default OFF, gated on a real workload | **STUDY** |
| Google Benchmark | Documented, not run on Zmbor here; same prior-evaluation note as Tracy | Apache-2.0 — compatible | Microbenchmarking | Benchmarking non-header-only kernels (e.g. port `vu1_interp_bench`); hand harness stays default | Optional harness | **STUDY** |
| Compiler-flag parity experiment (MSVC `/fp:fast`/AVX2 vs GCC/SSE4.1 through fpenv) | Design only | — | Flag comparison through the existing fpenv negative test | The 700 KB frame fact is MSVC `/Od` hwtest context, not a Linux/GCC measurement — do not generalise it | Experiment only | **STUDY** (no flag change recommended now) |
| Fast linkers (lld/mold) | Documented, not run here | Per section file | Faster linking | Link time not yet separated from compile time | Toolchain option | **STUDY** (only after separating link from compile time) |
| Profile-Guided Optimization (PGO) | Documented, not run here | — (compiler feature) | Profile-driven optimization | Runtime-only first; per-game generated-code PGO is a per-port cost (profiles do not transfer across games) | Gated on a real workload + named hotspot | **DEFER** |
| ThinLTO (as an independent item) | Documented, not run here | — (compiler feature) | Thin link-time optimization | Full LTO is **already on** in Release (`-O3 -flto=auto -fno-fat-lto-objects -ffp-contract=off -msse4.1`); "add LTO" is not a valid recommendation | Decide inside the Clang prototype | **DEFER** |
| Unity builds for generated code | — | — | Amalgamation | None: e2e single-file output already **is** unity; more unity worsens the documented failure modes | — | **REJECT** |
| LLVM/Clang optimization capabilities (general) | Documented | Apache-2.0 with LLVM exceptions | Optimizer for generated C++ | Applied via the Clang prototype, not by rebuilding LLVM | Toolchain | Covered by the Clang PROTOTYPE above |

Also recorded: `-ffast-math` is policy-forbidden for arithmetic-header code (fpenv negative
test, passed) — no flag experiment may reintroduce it there.

## 6. Category F — Reliability and verification

Section file: `sections/category-f-reliability-verification.md` (22 candidates, full template
entries, 8 experiment designs, concrete probe-workflow procedure). Grounding verified in
main: ASan/UBSan + 6 libFuzzer harnesses + Z3 integer proofs + mutant practice already
exist and have found real defects — but there is **no TSan/MSan/coverage wiring, and the
ps2autotests hwtest gate exists yet is not wired into CI** (no hwtest step in `build.yml`).
The 53/55 ladder baseline is historical, not re-run in this program. Prior Phase 7 verdicts
(FuzzTest, cvc5, AFL++-local, ps2sdk-blocked) were re-examined, not duplicated. **No
experiment in Category F was run — every magnitude below is a prior with a falsifier, not
a measurement.**

| Candidate | Verdict | Reason in one line |
|---|---|---|
| ps2autotests as a CI gate | **ADOPT** | Regression-only failure vs a re-established baseline; RSQRT/EFU as an explicit allowlist |
| Coverage reporting (report-only, no threshold) | **ADOPT** | Visibility without a gaming-prone gate |
| Native scheduled libFuzzer + corpus management | **ADOPT** | The no-Docker answer to OSS-Fuzz/ClusterFuzzLite |
| ps2sdk reference pinning table | **ADOPT** | Reading headers was never actually blocked by the Phase 7 toolchain/PS2 blockers |
| Metamorphic relations (recompilation determinism first) | **ADOPT** | Cheapest strong property: same input → byte-identical generated code |
| Hypothesis pilot (Python semantics models) | **ADOPT** | Property testing where the models already live (Python) |
| New libFuzzer targets (SIF packet parser first, then SPU2, then structure-aware analyzer fuzz after Phases 12/14 land) | **PROTOTYPE** | Concrete new failure classes; ordered by attack surface |
| TSan job (non-blocking first) | **PROTOTYPE** | Threading faults are the class ASan/UBSan cannot see; Windows parity UNKNOWN |
| Z3 translation validation + CFG checks | **PROTOTYPE** | Beyond the existing integer-semantics hook; FP encodings excluded until a hardware model is selected |
| Mull scoped pilot | **PROTOTYPE** | Mutation testing beyond the current manual practice |
| RapidCheck pilot (pure invariants) | **PROTOTYPE** | C++ property testing — but maintenance is partly-UNKNOWN (Dec 2023 packaged snapshots) and a BSD-2 vs BSD-3 licence-listing discrepancy must be resolved before adoption |
| SPU2 model-vs-runtime differential | **PROTOTYPE** | Model (Phases 6–11) vs runtime behaviour, with tolerances stated, not assumed |
| MSan | **STUDY (pilot only)** | Dependency-instrumentation + triage risk dominates |
| CBMC bounded pilot (DMA walk, clamp helpers only) | **STUDY (pilot only)** | Bounded proofs for two narrow, high-value surfaces |
| PCSX2 differential as manual triage only | **STUDY** | Never a gate, never a fix driver (Zmbor's standing rule: reference, never evidence) |
| Curated clang-tidy run | **STUDY** | Lint signal, triaged, not enforced blindly |
| AFL++ | **DEFER (trigger: libFuzzer corpus plateaus; external-only forever under AGPL-3.0)** | Licence + marginal value over existing fuzzing |
| FuzzTest | **DEFER** | Revisit only if the Phase 7 reasons no longer hold |
| cvc5 | **DEFER (reopen on first Z3 `unknown`)** | Second solver only when Z3 actually fails |
| KLEE | **DEFER** | Path explosion on whole-machine state |
| ClusterFuzzLite as-is | **REJECT** | Its docs state Docker as a prerequisite; the no-Docker constraint disqualifies it outright (only its workflow ideas survive, as native libFuzzer scheduling) |
| OSS-Fuzz hosted | **REJECT** | Eligibility unproven for a young single-maintainer project, Docker-based, plus disclosure burden |

Probe-workflow procedure (Section 4 of the F file, procedure only — no results, no PS2
available): ps2dev prebuilt tarballs as the no-Docker route; probe questions P1–P5 plus
queued P6 SPU2 / P7 COP0 / P8 SIF0; capture-then-ingest discipline; no `.expected` edits;
no promotion of emulator observations to hardware facts.

## 7. What this landscape deliberately does not contain

- No commercial-game evidence, no game binaries examined, no game-specific code.
- No candidate whose only support is popularity, vendor speedups, or a single game's fps
  table presented as a framework result.
- No Docker-based workflow, no paid/closed tooling as an adoption target, no licence-
  unresolved code reuse.
- No replacement for Zmbor's existing discovery, CFA, merge order, CPU rasteriser, or
  runtime — every ADOPT/PROTOTYPE above attaches to an existing seam or adds an optional
  input, and the design document states what stays in full.
