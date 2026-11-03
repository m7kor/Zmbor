# Zmbor core-mission assessment: the recompilation pipeline, audited end to end

**Type:** AUDIT ONLY — no product source code was changed by this assessment.
**Baseline:** `350ca1f3b2cd2aede0d5ec8491b36d2c8a552141` (2026-10-28).
**Working tree at audit time:** `~/workspace/zmbor-work/Zmbor`, 36 files vs baseline
(the Phase 10b guest-register tree; Phase 10b was still being edited while this
audit read the tree, so all findings below rest on code paths that predate 10b
unless stated, and no finding depends on 10b's uncommitted edits).
**Date:** 2026-11-03. **Remote repo:** untouched. **No `docs/status/components.json`
status was changed, upgraded, or re-measured.**

## Mission restated (Gabriel, 2026-11-03)

Zmbor is a PS2 **static recompilation and native game-porting framework**, not a
conventional emulator. The pipeline is:

**PS2 Game (MIPS R5900 ELF) → Zmbor Analysis → Generated C++ →
Native Compilation → Playable PC Port.**

Emulated subsystems (VU, GS, SPU2, IOP, DMAC, SIF) are supporting infrastructure
for the generated native code, not the final product. Priorities in order:
(1) recompiler accuracy/completeness/performance; (2) automatic ELF analysis,
function discovery, control-flow reconstruction, code generation, linking;
(3) reliable native runtime; (4) VU/GS/SPU2/IOP/DMAC/SIF only as required for
correct execution; (5) reduce interpreter overhead where justified, never at
correctness's expense; (6) automated game-to-C++ project generation;
(7) profile generated programs and optimize measured bottlenecks;
(8) validate end-to-end with legally obtained homebrew and game assets.
No ISO-launching emulator direction. Never claim an arbitrary commercial game
converts automatically until demonstrated by reproducible end-to-end testing.

## Method and a warning about the dashboard

This audit read code, not summaries: `ps2xRecomp` (elf_parser, control_flow_
analyzer, ps2_recompiler, code_generator, instruction translators, types.h),
`ps2xAnalyzer` (elf_analyzer, analysis_passes, elf_analysis_context,
sce_symbol_scanner, toml_generator), `ps2xRuntime` (main, memory, VIF/VU/GS/
SPU2/IOP integration, audio backend, stubs), `ps2xTest/e2e`, and
`docs/engineering/AUTONOMOUS_EXECUTION_STATE.md`,
`PS2_COMPONENT_GAP_MATRIX.md`, and `docs/status/components.json` (read only).

**Warning — the dashboard lags the tree.** `components.json` still lists
`spu.voices`, `spu.adsr`, `spu.pitch`, `spu.mix`, `spu.reverb` as MISSING and
`dma.c5` as HLE_OR_STUB, even though Phases 4, 6–11 implemented SIF0, ADSR,
pitch, mixing, reverb, and ADMA in the working tree. That is by the standing
rule (statuses are never upgraded by these cycles without new evidence), but it
means the dashboard must NOT be used to rank SPU2 work: this audit ranks from
code. The component totals in `components.json` do check out as a historical
count: 227 components — 22 VERIFIED_COMPLETE, 81 VERIFIED_PARTIAL,
77 IMPLEMENTED_UNVERIFIED, 20 HLE_OR_STUB, 15 MISSING, 9 UNKNOWN,
3 NOT_APPLICABLE (the PASS/MISMATCH entries in the same file are test-result
records, not components).

**Tests actually run by this audit:** `ps2x_tests` from `/tmp`: **620/620
passed, 0 failed** (Release build in `~/workspace/zmbor-build`); `ctest -N`
in the same build dir lists **55 tests total** (the 54-passed/1-skipped-z3
CTest result is the Phase 10/11 verified baseline, cited as prior evidence,
not re-run end-to-end by this audit). No stale-binary run was counted: the
binary run is the current Release build; a VM replacement during the audit
window forced the parallel Phase 10b workstream to reinstall X11/GL headers,
which did not affect this audit's read-only results.

## What the pipeline demonstrably does today

The pipeline is real, not aspirational, at small scale:

1. **Assemble a guest ELF without a PS2 toolchain.** `ps2xTest/e2e/
e2e_programs.py` builds four tiny MIPS programs in Python (`basic`,
`gs_sprite`, `branchdelay`, `vu_macro`) and `mips_elf.py` emits a real ELF.
2. **Analyse.** `ps2_analyzer <elf> config.toml` (optional) generates a
starting configuration; `ps2_recomp config.toml` runs the recompiler.
3. **Generate C++.** `ps2_recompiled_functions.cpp/.h`,
`ps2_recompiled_stubs.h`, `register_functions.cpp` are emitted, including a
function table (`function_table_emitter.cpp`) for runtime dispatch.
4. **Compile natively and run.** The generated sources link against
`ps2_runtime` with `ps2xRuntime/src/main.cpp` as the entry point; `run_e2e.
cmake` checks exit code and exact stdout, and for `gs_sprite` dumps the
presented frame and pixel-checks it (`check_frame.py`). These 4 e2e tests pass
in the established baselines (they fail headless only for lack of DISPLAY;
they pass under `xvfb-run -a`).
5. **Hardware-result validation exists as a second ladder.**
`tools/hwtest/hwtest.py` recompiles ps2autotests programs and compares output
against real-PS2 `.expected` files. Historical baseline, unchanged `.expected`
files: **53/55 programs, 8202/8250 lines**; known failures `cpu_ee_fpu_sqrt`
59/66 (RSQRT.S, 7 lines off by 1 ulp) and `vu_lower_efu` 182/223 (EFU last
bits) — both blocked on physical-PS2 probes that do not exist here. IOP
programs: 23/23 exact, 1306/1306 lines.

So the honest statement of current state is: **ELF → analysis → C++ → native
build → run is demonstrated for hand-built micro-programs and for hardware
test programs; it is NOT demonstrated for any real game, stripped or not.**
The gap between those two sentences is the ranked list below.

## Pipeline stage map (evidence status per stage)

| Stage | Status | Evidence (file/line, behaviour) |
|---|---|---|
| ELF parsing (32-bit MIPS, sections, symbols, relocations, Ghidra map) | IMPLEMENTED, TESTED | `ps2xRecomp/src/lib/elf_parser.cpp` (2,692 lines); Phase 3 added 64-bit section-end helpers and rejection of non-32-bit ELFs. Unit tests: `elf_analyzer_tests`, `r5900_decoder_tests`. |
| Function discovery | IMPLEMENTED, PARTIAL — see Blocker 1 | `elf_parser.cpp::extractFunctions` builds functions **only** from symbols + `m_extraFunctions` (Ghidra map / config). No recursive-descent discovery from the entry point was found. |
| SCE SDK symbol identification | IMPLEMENTED, TESTED | `ps2xAnalyzer/src/elf_analyzer.cpp::discoverSceSdkSymbols` + `sce_symbol_scanner.cpp` (793 lines) + embedded `sce_symbol_database_data.h` (~12 MB header in `ps2xAnalyzer` — avoid raw greps). Pattern-matches known SDK functions into the symbol list. |
| Analysis passes (data usage, patch detection, control flow, jump tables, performance, recursion) | IMPLEMENTED, TESTED on small inputs | `elf_analyzer.cpp::analyze()` runs `runDataUsagePass`, `runPatchDetectionPass`, `runControlFlowPass`, `runJumpTablePass`, `runPerformancePass`, `identifyRecursiveFunctions`; `analysis_passes.cpp` (444 lines). |
| Control-flow reconstruction (direct branches, delay slots, jump tables, indirect jumps) | IMPLEMENTED, PARTIAL — see Blocker 2 | `control_flow_analyzer.cpp` (463 lines) + `control_flow_emitter.cpp` (754 lines) + `jump_table_switch_emitter.cpp`; delay-slot extension is handled (`extendSliceOverDelaySlot`, `ps2_recompiler.cpp`). Indirect JR/JALR resolved via configured tables or kept as fallback entry points. |
| Code generation (EE core, MMI, FPU, COP0, VU macro) | IMPLEMENTED, TESTED on small inputs | `code_generator.cpp`, `instruction_translator.cpp` (dispatches SPECIAL/REGIMM/COP0/FPU/VU/MMI/memory), `special_translator.cpp`, `mmi_translator.cpp`, `fpu_translator.cpp`, `cop0_translator.cpp`, `vu_translator.cpp`. Unhandled opcodes emit a runtime `throw` (`code_generator.cpp::emitUnhandledInstruction`) and are recorded by the reporter — a loud failure, which is the right behaviour. |
| VU micro programs | INTERPRETED, not recompiled — by design | VU0/VU1 micro code loaded via VIF MPG runs in `VU1Interpreter` (`ps2xRuntime/include/runtime/ps2_vu1.h`); only VU0 *macro* (COP2) instructions in EE code are translated (`vu_translator.cpp`). The hybrid is deliberate; its performance cost on real VU workloads is UNMEASURED. |
| Linking / function table / stubs | IMPLEMENTED, TESTED on small inputs | `function_table_emitter.cpp` emits `g_ps2RecompiledFunctionTable` with base/end/slot count; `ps2_recompiler.cpp` emits stub bodies dispatching to `ps2_syscalls::` / `ps2_stubs::` handlers or `TODO_NAMED` — see Blocker 3. |
| Native runtime (memory, kernel syscalls, VIF/VU/GS/SPU2/IOP, DMA, SIF) | IMPLEMENTED, PARTIAL | `ps2xRuntime/src/lib/` (memory, VIF1 interpreter, GS CPU frontend/backend, IPU, IOP host, pad, VFS, kernel stubs). GS backends: CPU rasteriser only (`gs_cpu_backend.cpp`); Vulkan MISSING, parallel-gs MISSING. |
| Game-to-C++ project generation (priority 6) | MISSING as automation | Workflow is manual: analyzer → hand-edited `config.toml` → `ps2_recomp` → hand-wired runtime build (README "Usage"). `ps2xStudio` is a GUI (`GUI.cpp`, `main.cpp`, `ui/`), not a batch project generator; no project generator was found in `tools/`. |
| Profiling generated programs (priority 7) | NOT STARTED (UNKNOWN on real inputs) | No profiling harness for generated native programs was found. One measured symptom exists: unoptimized recompiled functions keep every local in its own stack slot, with frames up to ~700 KB measured, requiring a 64 MB stack reserve on MSVC (`ps2xTest/e2e/CMakeLists.txt` comment). Codegen quality on real inputs is therefore UNKNOWN, not good or bad. |
| End-to-end validation on real games | UNVERIFIED (no claim made) | e2e micro-programs + ps2autotests only. No commercial game has been run end to end through this pipeline in any evidence reviewed. |

## Ranked gaps, by end-to-end blocking impact

Ranking rule: a gap outranks another if it breaks an *earlier* pipeline stage
or a *broader class of programs*. Subsystem completeness in isolation does not
rank.

### 1. Stripped-ELF function discovery — the pipeline's front door is closed
**What breaks:** any ELF without a symbol table. `extractFunctions`
(`ps2xRecomp/src/lib/elf_parser.cpp:1764`) builds the function list only from
symbols (symtab/dynsym) plus `m_extraFunctions` (Ghidra map, config entries);
sizes default to "next function start or section end" when a symbol has no
size. The analyzer's entry-point handling (`elf_analyzer.cpp::
analyzeEntryPoint`) only *forces* a start when the entry address already maps
to a function, with a fallback to a function at `0x100000`/`0x80100000`.
Shipped game ELFs are normally stripped; homebrew built without `-g`/symtab
is the same. For those inputs, discovery yields only SCE-DB pattern matches
plus whatever a human supplied via Ghidra or config — i.e. the "automatic"
pipeline is manual at step one. **Next milestone:** see "Recommended next
milestone" below — it is this gap, because it is the smallest demonstrable
advance that unblocks a whole class of inputs.

### 2. Indirect control flow without manual configuration
**What breaks:** switch statements and function-pointer dispatch in real code.
`control_flow_analyzer.cpp:314` resolves jump tables from
`m_configJumpTableTargetsByAddress` — tables the user configured in TOML
(`config_manager.cpp` `jump_tables` section). The analyzer has a jump-table
pass, but unconfigured tables fall back to `indirectFallbackEntryPoints`
(`control_flow_analyzer.h:21`), kept as registered resume entry points for
runtime function-table dispatch rather than proven CFG edges. A missed table
target is therefore not a clean error: it is wrong control flow or a missed
function at runtime. Switch-heavy game code (state machines, message
dispatch) is exactly where this bites. **Next milestone after #1:** make the
jump-table pass configuration-independent (detect LUI/ADDU/SLL/JR table
patterns from data flow, verify targets land in executable sections, and
report unresolvable indirect jumps as analysis failures instead of silent
fallbacks).

### 3. Unhandled library/syscall surface fails silently (returns −1)
**What breaks:** diagnosis and correctness of any game that calls an
unhandled function. Unknown stub functions become `ps2_stubs::TODO_NAMED`
(`ps2_recompiler.cpp:1294,1322`); `TODO_NAMED`
(`ps2xRuntime/src/lib/Kernel/Stubs/Unimplemented.cpp:11`) logs one warning
per name (then suppresses), sets the return register to −1, and **continues
execution**. The analyzer already classifies library functions into handled
vs `m_untrackedStubFunctions` (`elf_analyzer.cpp::analyzeLibraryFunctions`),
so the set of silent failures is knowable per game — but today a game that
depends on an unhandled call runs on with a bogus failure code instead of
stopping. That inverts the pipeline's own good rule (unhandled *instructions*
throw loudly; unhandled *calls* whisper). **Next milestone:** per-game stub
coverage report emitted by the analyzer (handled / stubbed / unknown counts
with names), plus a strict mode where an unknown stub call aborts with the
call site, so "it ran" can never mean "it silently skipped work".

### 4. GS rendering has no performant backend
**What breaks:** playable ports at real scene complexity. The only GS backend
is the CPU rasteriser (`ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp`,
`gs_frontend.cpp`); `gs.vulkan` and `gs.pgs` are MISSING in the gap matrix,
and `gs.genregs`, `gs.tex`, `gs.prims`, `gs.swizzle`, `gs.vram` are all
IMPLEMENTED_UNVERIFIED. The e2e `gs_sprite` test proves one sprite frame
pixel-for-pixel — it proves the path exists, not that it runs a scene at
speed. A port that renders at 2 fps is not a playable port. **Next
milestone:** GS performance baseline on a synthetic scene (draw-call count
vs frame time for the CPU backend) to decide whether Vulkan/parallel-gs is
actually the next GS step or whether the CPU backend suffices for the first
target title; do not start a Vulkan backend on assumption.

### 5. COP0 TLB / cache / exception model is absent or unverified
**What breaks:** any game using TLB-mapped addresses, CACHE operations, or
relying on exception delivery. Gap matrix: `cop0.tlb` — "No TLB-mapped address
translation or refill/invalid exceptions; no test"; `cop0.cache` — MISSING
("CACHE ops are translated without a cache model; coherency effects are not
emulated"); `cop0.exc` — IMPLEMENTED_UNVERIFIED; `cop0.adderr` — "Fast-path
memory helpers do not check alignment". Fast-path memory (`mem.fast`
VERIFIED_PARTIAL) is the hot path, so this is a silent-misbehaviour risk,
not just a missing feature. **Next milestone:** TLB translation for the
common case (wired entries + a minimal refill path) with tests driving a
guest access through a mapped address, before any game that uses mapped
memory is attempted.

### 6. VU execution is synchronous; VBS and D/T interrupts are never delivered
**What breaks:** games that synchronize EE code with VU1 micro programs.
Phase 8 established this and it stands: VU runs synchronously, VBS is never
observable, D/T INTC are never delivered; macro MAC/STATUS flags await probe
P5 (generated, unmeasured). The e2e `vu_macro` program passes, but it is a
single macro program, not an asynchronous VU1 workload with DMA timing.
**Next milestone:** EE-scheduler concurrency scaffolding that makes VBS
observable and delivers D/T interrupts, gated by a test with a VU1 program
that signals completion — not a VU core rewrite.

### 7. Generated-code performance is unmeasured (priority 7 unstarted)
**What breaks:** nothing yet — which is why it ranks below correctness
blockers. There is no profiling of generated native programs, so claims
about "interpreter overhead" or "native speed" are currently unsupported in
both directions. The one measured symptom (700 KB unoptimized stack frames,
64 MB stack reserve workaround in e2e CMake) suggests codegen quality work
(register allocation / local lifetime) may matter more than any interpreter
removal, since VU micro code is interpreted by design and EE code is already
native. **Next milestone:** profile the e2e + hwtest generated programs
(frame time, stack use, hot functions) and publish the first measured
bottleneck list before optimizing anything.

### 8. No automated game-to-C++ project pipeline (priority 6)
**What breaks:** adoption and repeatability, not correctness. Every port
today is a manual sequence (analyzer → hand-edited TOML → recompiler →
hand-wired CMake target against `ps2_runtime`, as in `ps2xTest/e2e/
CMakeLists.txt`). `ps2xStudio` does not fill this: it is a GUI, not a batch
generator. This ranks below the correctness blockers because a manual
pipeline that produces a correct port beats an automated pipeline that
produces a broken one — but it is the first workflow milestone once #1–#3
land, since it is what turns one-off ports into a repeatable process.

### 9. SIF1/SIF2/DMA-timing gaps (IOP-heavy games)
Gap matrix: `dma.c6` (SIF1) — "IOP channel 10 interrupt is not raised",
SIF FIFO depth/timing not modelled; `dma.c7` (SIF2) — MISSING;
`dma.timing` — HLE_OR_STUB ("No bus arbitration or transfer timing").
SIF0 is the exception: modelled both ends in Phase 4, though its production
runtime push-notification wiring was explicitly out of that slice and
remains to be verified in the runtime path. Ranked below #1–#6 because most
game logic does not hinge on SIF2 or bus timing, but IOP-communication-heavy
titles will hit it.

### 10. RSQRT/EFU last-bit mismatches (hardware-probe-blocked, narrow)
`cpu_ee_fpu_sqrt` 59/66 and `vu_lower_efu` 182/223; 8202/8250 lines overall
(99.4%). These affect specific float programs, not pipeline throughput, and
cannot be closed without a physical PS2 probe. They rank last by design:
chasing 1-ulp fidelity before a first game runs would invert the mission's
own priorities.

## Audio-sink evaluation (explicitly requested)

**Verdict: the audio sink ranks BELOW the top 5 blockers, and below #6–#8
for sequencing purposes. It is not unimportant; it is not next.**

Reasoning, in order:

1. **A silent game still runs.** Graphics, input, game logic, and save/load
are unaffected by audio. A port that plays silently is a playable port with
a known defect; a port that cannot discover its functions, resolves its
switches wrong, silently stubs its library calls, renders at 2 fps, or faults
on mapped memory is not a port at all. The sink therefore cannot outrank
Blockers 1–5 on end-to-end impact.
2. **No recompiler stage depends on it.** Nothing in ELF parsing, analysis,
CFG reconstruction, code generation, or linking needs audio. The sink is a
runtime output device, downstream of everything that blocks.
3. **The missing piece is narrower than "no audio".** The runtime already
has a host-side VAG path (`ps2xRuntime/src/lib/ps2_audio.cpp`, decoded VAG
via the raylib host audio device in `ps2_runtime.cpp`), used by HLE sound
commands. What is missing is specifically the **IOP SPU2 `mixTick` output
→ host audio stream** sink: Phases 9–11 produce mixed frames and count them;
nothing plays them. That is a small, well-bounded integration (stream
`mixTick` frames into the existing host audio device), not a research
project — which is precisely why it should not displace research blockers:
it can be done cheaply later, and doing it now buys nothing for the
pipeline.
4. **What would have to be true for the sink to outrank the blockers:**
(a) a real title runs graphically end to end through the pipeline and
silence is the remaining user-visible defect; or (b) the chosen first target
title is audio-critical (music/rhythm) such that silence defeats its
purpose. Until either condition holds, sink *fidelity* work (39-tap FIR,
PS1 presets, MMIX effect-path feed, volume sweeps) is emulator polish, and
even the basic sink should wait behind Blockers 1–3.

## SPU2 / ADMA / SIF workstreams through the mission lens

**Already unblocking recompiled execution (keep, do not regress):**

- SPU2 register file, 2 MB sound RAM, data port, IRQA, IOP DMA channels 4/7
(Phases 1–3, 12 in the old batch numbering): the surface a generated
program's IOP modules actually touch.
- Voice engine, ADSR, pitch/Gaussian, mixing, core routing (Phases 6–9):
without these, a generated program that keys a voice produces nothing or
wrong output. These were correctly prioritized as infrastructure.
- SIF0 both-ends vertical slice (Phase 4): IOP→EE DMA with EE completion
(CIS5 + INT1) and IOP completion (IRQ3 path) is infrastructure a generated
program's SIF traffic needs. Its remaining item that matters for execution
is verifying the production runtime push-notification wiring that Phase 4
left out — not more SIF0 model detail.
- Guest-register integration (Phase 10b, in progress): wiring the
ps2sdk-pinned P/R-block map so guest code can drive MVOL/EVOL/parameters/
ESA/EEA is execution-unblocking in a way the model API alone was not. This
is the last SPU2 item that changes what generated code can do, and it
should finish.
- ADMA stream model (Phase 11): the sound-data input path exists. The one
ADMA item that matters for execution is still open: the guest ADMA enable
register offset remains unpinned, so guest code cannot yet drive ADMA. That
pinning task (a retrieved PCSX2/ps2sdk source) outranks any ADMA geometry
refinement.

**Emulator completeness beyond what generated code needs (deprioritize):**

- Reverb fidelity: 39-tap FIR resampler, PS1 presets, MMIX effect-path
feed, RAM-visible work-area writes, ATTR write-enable subtleties beyond
the bit-7 gate, volume sweeps, noise generator. None of these changes
whether a generated program runs; all of them can wait until a title's
audio is audibly wrong in a way a user can demonstrate.
- SIF2 modelling, DMA bus-timing fidelity, and SIF1 FIFO-depth modelling,
unless a chosen target title demonstrably needs them (see Blocker 9).

**What should pause:** further audio-fidelity milestones and any new
emulator-completeness workstream that is not tied to a target title's
demonstrated failure. The next audio-related work after 10b should be the
basic SPU2 sink — and only when Blocker 1–3 work is underway or a
graphical title runs (see the audio-sink evaluation).

## Recommended next implementation milestone (ONE)

### Stripped-ELF function discovery v1

**Why this one:** it is the smallest demonstrable advance that unblocks a
whole class of inputs (Blocker 1), it touches no working subsystem's
semantics (additive discovery; symbol-derived functions stay authoritative
where present), and its success is mechanically checkable (discovered set
== expected set on a known input).

**Scope (deliberately narrow):**

- Recursive descent from the ELF entry point: follow direct JAL/J branches,
splitting functions at call targets and at return boundaries; infer a
function's end from the next discovered function's start or the section
end, reusing the existing 64-bit section-end/clamp helpers in
`ps2xRecomp/include/ps2recomp/types.h` (Phase 3) so the 0xFFFFFFFF wrap
class stays fixed.
- Jump-table recovery for the common pattern (LUI + ADDU + SLL + JR) using
the analyzer's existing jump-table pass shapes, without requiring TOML
configuration; targets must land in executable sections or be rejected.
- Merge order (non-negotiable): symbols > Ghidra map > config extras >
discovered functions. Discovered functions never overwrite a
symbol-derived range. Unresolvable indirect targets are reported by the
analyzer as failures, not silently kept as fallback entry points.

**Files:**

- `ps2xRecomp/src/lib/elf_parser.cpp` — new discovery pass (or extension
of `extractFunctions`); `ps2xRecomp/include/ps2recomp/elf_parser.h` for its
interface.
- `ps2xAnalyzer/src/elf_analyzer.cpp` — entry-point descent wiring and
merge-order enforcement; `ps2xAnalyzer/src/analysis_passes.cpp` if the
jump-table pass needs the new shapes.
- `ps2xRecomp/src/lib/control_flow_analyzer.cpp` — accept
analyzer-discovered tables the same way configured tables are accepted
today, with identical validation.
- Tests only, no product-code changes elsewhere.

**Tests:**

- New `ps2xTest` cases (`elf_analyzer_tests.cpp`, `ps2_recompiler_tests.cpp`):
a Python-assembled ELF in the `e2e_programs.py` style, built with symbols
and then stripped of them, whose expected function set is known exactly;
the test asserts discovered == expected (starts and ends), and asserts the
merge order (a planted symbol range survives discovery unchanged).
- New e2e program (`stripped`, in `ps2xTest/e2e/`): the stripped ELF goes
through `ps2_recomp` → native build → run → exact-output check, proving
the discovered functions actually execute.
- Regression: existing e2e 4/4, `ps2x_tests`, CTest, IOP suites — no
regression allowed. Mutant: delete one call edge from the synthetic ELF;
the test must then miss exactly that function (proves the test is not
vacuous).

**Verification plan (standing convention):** Release and ASan+UBSan builds
(`~/workspace/zmbor-deps` cache + `FETCHCONTENT_SOURCE_DIR_*` overrides,
Unix Makefiles, Studio/FFmpeg off; reinstall X11/GL headers if a VM
replacement wipes them; never count a stale-binary run). IOP 4/4 both
modes, `ps2x_tests` 620/620 both modes (or the then-current baseline — the
count, not the number, is the gate), CTest 54 passed / 1 skipped (z3) /
0 failed under `xvfb-run -a` both modes. Deliverables per convention:
individual patch on top of the then-current cumulative patch plus a
cumulative patch alone at baseline, both verified on fresh anonymous
clones with tree comparison, plus a ledger section. No
`components.json` status change (discovery is a framework capability, and
statuses change only with the evidence the dashboard methodology requires).

**Explicitly NOT in this milestone:** a recompiler rewrite, a new IR, a
Vulkan backend, a TLB model, a project generator, or an audio sink. Each
has its place in the ranked list; none of them unblocks stripped ELFs.

## Limitations of this audit

- No real game ELF was available to or run by this audit; all
"breaks on real games" claims are inferences from code structure (symbol
dependence, config dependence, stub semantics), stated as such.
- No physical PS2, so RSQRT/EFU and all hardware-fidelity questions stay
open and are ranked accordingly, not dismissed.
- Phase 10b was in progress during the audit; its guest-register routing
was treated as pending, not as established fact.
- The 620/620 count is the current tree's unit-test baseline, not a
measure of game compatibility; the report never converts it into one.

## Bottom line

The pipeline works at micro scale and the infrastructure underneath it
(SPU2 voice path, SIF0, runtime, function table, stubs) is substantially
built. What stands between micro scale and a first real port is not audio
fidelity and not another subsystem: it is **getting functions out of a
stripped ELF, proving indirect control flow without hand configuration,
and stopping silent stub failures from masquerading as execution**.
Fix discovery first; everything else in the ranked list gets easier to
measure once real inputs flow through it.
