# Zmbor Stabilization — Root Cause Audit (Gate G, final integration review)

**Date:** 2026-11-04 (Asia/Riyadh). **Scope:** the Gate A defect register
(S1–S14) with final dispositions after Gates B–F, plus the Gate G review of
every file in the publication (forward) stack. This document is a review
record staged outside the repo tree; it is not a tree change and does not
alter any component status.

**Evidence vocabulary (used throughout):** IMPLEMENTED = code present in the
tree; TESTED = covered by a test that runs in the suite; VERIFIED = the
test evidence was actually run and checked (by the gate coordinator and,
for load-bearing claims, re-run independently by the main agent);
HARDWARE-CONFIRMED = demonstrated against physical PS2 hardware or an
authoritative hardware source; UNVERIFIED = could not be checked here,
with the blocker stated; UNKNOWN = not established either way.

**No hardware claim is made anywhere in this document.** No component
status in `docs/status/components.json` is upgraded by stabilization work.

## 1. Register dispositions (Gate A S1–S14)

### S1 — P0 — `ps2_memory.h` missing `<string>` — FIXED (Gate B), MSVC outcome UNVERIFIED

- Root cause (confirmed by code reading): line 24 used `std::to_string`
  while the header's include list had no `<string>`; GCC/Clang accepted it
  only through libstdc++'s transitive `<iostream>` → `<string>`.
- Fix: `#include <string>` added to `ps2xRuntime/include/runtime/ps2_memory.h`
  (line 15 in the current tree; `<sstream>` added for S7 sits at line 13).
  Nothing else in that fix.
- Evidence: Gate B header check pre-fix (GCC) 71 headers, PASS=60 FAIL=11;
  post-fix PASS=71 FAIL=0 in both dependency layouts. Suite 651/651 both
  modes at Gate B. The fix addresses the exact code defect behind the CI
  job failure (run 41668778457, `windows-msvc-x86_64`, Build step).
- UNVERIFIED: whether the MSVC diagnostics vanish. Anonymous job-log access
  returns 403, and no MSVC toolchain exists on this VM. The publication-run
  procedure is in `CROSS_PLATFORM_VERIFICATION.md`.
- C2512 classification: secondary cascade of the ill-formed base
  initializer, NOT an independent defect — unless it survives a `<string>`
  fix in CI, in which case Gate B's binding rule reclassifies it as
  independent. That reclassification trigger is carried, not assumed away.

### S2 — P1 — Header self-containment — FIXED (Gate B), MSVC/Clang legs UNVERIFIED

- Fixed at ownership boundaries: `types.h` (ps2xRuntime) gained `<cstddef>`;
  `control_flow_utils.h` gained `#include "ps2recomp/types.h"` (no cycle);
  `ps2_stubs.h` no longer includes private
  `src/lib/Kernel/Stubs/Unimplemented.h` — the TODO/TODO_NAMED declarations
  moved into the public header, and `Unimplemented.h` is now a shim for its
  in-tree includers (`Common.h`, `Unimplemented.cpp`). The `ps2_runtime`
  PUBLIC export of `src/lib/Kernel` was deliberately left untouched (see S13).
- Scope extension, stated plainly by Gate B: the same missing-`<cstddef>`
  pattern was found and fixed in 7 further headers (`ps2_log.h`,
  `ps2_runtime.h`, `elf_analyzer.h`, `ps2_recompiler.h`, `iop_subsystem.h`,
  `gs_cpu_backend.h`, `ps2_ipu.h`) — one line each, each proven by the
  check, no other includes added anywhere. Gate G re-verified all nine
  `<cstddef>` additions (including the corrected
  `ps2xAnalyzer/include/ps2recomp/elf_analyzer.h` path) and the
  `control_flow_utils.h` include in the current tree.
- Regression mechanism: new tree file `tools/check_public_headers.py`
  (stdlib-only): per-header standalone compilation plus a curated
  direct-include audit (`to_string` → `<string>`, `size_t` → `<cstddef>`),
  which is what catches S1 under GCC where standalone compilation passes
  via transitive includes. It accepts both dependency layouts
  (`<deps>/<name>` and `<deps>/<name>-src`) and compiles
  `gs_parallel_backend.h` with `-DPS2X_HAS_PARALLEL_GS=1`. Gate G re-ran
  it locally against the dependency cache: **71/71 PASS under g++**.
- UNVERIFIED: MSVC and Clang standalone results (no toolchains on this VM;
  CI legs cover Clang, but the local Clang header-check leg was never run).

### S3 — P1 — SLTIU sign-extension defect — FIXED (Gate C)

- Root cause (confirmed by code reading + counterexample):
  `ResolveBlockConstant` compared against the zero-extended 16-bit field
  (`imm16`) instead of the sign-extended immediate. `sltiu $t0, $s0, -1`
  with `$s0 = 0x10000` must yield 1 (`0x10000 < 0xFFFFFFFF`); the old code
  yielded 0 (`0x10000 < 0xFFFF`).
- Fix: the SLTIU arm now compares `*known[rs] < static_cast<uint32_t>(simm)`,
  mirroring the SLTI arm, with a comment stating the semantics and the
  counterexample. Gate G re-read the fixed arm in the current tree; it
  matches the Gate C description exactly.
- Evidence: 3 new tests (negative immediates −1/−32768, `rs` 0 and
  `0xFFFFFFFF`, SLTI-vs-SLTIU transitions, proof-invalidation cases);
  fail-before 653/654 (exactly the negative-immediate test); mutant check
  (revert to `imm16`) fails exactly that test. Jump-table bound SLTIU
  sites untouched — they are fail-safe via the >1000 cap, not wrong.
- Neighbours unchanged and correct: SLTI, ADDIU/ADDI, ORI/ANDI/XORI, LUI.

### S4 — P1 — GS CLUT restoration insufficient — FIXED (Gate E, Option 2)

- Root cause (confirmed by code reading): swap re-issued only the last
  `LoadClut` call's arguments while CPU-backend palette state (`m_clut[]` +
  `m_clutCbp[0..1]`, zero-initialized per backend) is cumulative and
  CLD-conditional. Three provably failing sequences: CLD=1 → CLD=0 no-op
  last; CLD=4/5 with CBP=0 on a zeroed backend; multiple CBP/CSA slices.
- Decision: **ordered full-history replay**. Startup-only selection was
  rejected on evidence: mid-stream swap is a load-bearing, tested Stage 0
  capability, and the GS constructor always installs a CPU backend, so
  every `setRasterBackend` is technically a swap — no clean first-vs-
  mid-stream distinction exists to enforce. Cross-backend snapshot copying
  was never used (forbidden by Gate A).
- Implementation (Gate G re-read the swap path): every delivered
  `(tex0, texclut)` pair is recorded in order (`m_loadClutHistory`),
  including no-ops and partial slices, after the raw-mode early return;
  history clears on `reset()`; at swap it replays in order through the
  fresh backend's own public `LoadClut`, skipped for raw backends, never
  routed through `issueLoadClutUnlocked` (that would double history).
  History is deliberately not compacted (compaction would duplicate
  backend CLD semantics). The old last-load fields were removed outright.
- Evidence: 5 fail-before/pass-after sequence tests (S4a, S4b, S4c, S4d,
  CSM2 — the last closing the Stage 1 CSM1-only fixture gap), pixel-compared
  before vs after swap; fail-before 678/683 (exactly the five S4 tests);
  6 guards/pins passing pre- and post-fix as designed (TEXFLUSH snapshot
  semantics, reset rule, raw-mode parity, three §5.2 pins). TEXFLUSH not
  touching `m_clut` remains correct snapshot semantics, not a bug.
- Remaining limitations (carried, not fixed): history grows one record per
  TEX0/TEX2 write within a reset epoch; replay reads current VRAM, not
  load-time snapshots (pinned by the TEXFLUSH guard); raw-mode decoded
  history is not recorded (parity, pinned).

### S5 — P1 (verification item) — Scheduler transfer / C++ lifetime — PROVEN-SAFE (Gate C)

- Framing correction (confirmed by grep + code reading): no C
  `setjmp`/`longjmp` exists in product code. `EeDispatcherTransfer` is a
  plain C++ exception caught only at `EeScheduler::run()`; destructors run
  during unwinding, so the longjmp-skips-destructors UB class does not
  apply as implemented. The Phase 15 "scheduler longjmp" description was
  corrected in place during Gate A.
- R1 (noexcept boundary → `std::terminate`): PROVEN-SAFE. Full-chain audit:
  generated code → `eeCheckpointDue` (noexcept) → `checkpointDue` →
  `accountCycles` → `advanceEeTimers` + `advanceIopEeCycles` →
  `runEeCycles` (noexcept; `runCycles` is noexcept AND swallows everything
  in `try/catch(...)`) + map-find/iteration/arithmetic/atomic-load links;
  no link can throw; the production `catch(...)` rethrow in `run()` is
  caught by the gameThread lambda — never a terminate.
- R2 (swallow sites losing state): PROVEN-SAFE. The transfer carries no
  state; state lives in the live context the loop re-reads — site 222
  receives the live `activeContext()`; site 262 runs after the completed
  invocation is popped; all existing `onComplete` callbacks audited (IPU,
  SIF, RPC, System, Thread, EeScheduler) write to live parent/host state.
- Evidence: 4 adversarial tests (RAII destructor runs during unwinding +
  dormant context holds vector/EPC/Cause/AdEL/BadVAddr/EXL; onComplete-raised
  BREAK survives site 262; resumeCompletion-raised BREAK survives site 222;
  interrupt-path fault abandons cleanly, continuation runs exactly once).
  Real finding folded into the tests: a function cannot be registered at
  the KSEG0 vector `0x80000080` (dense table `[0x0, 0x2000000)` only), so
  after an abort the thread goes dormant via the designed missing-function
  path — the tests assert that designed fallback.
- Caveat (carried): the verdicts cover existing paths only. A future
  noexcept-boundary introduction or a new callback that raises into a dead
  context needs re-audit. Full case in `RECOMPILER_RUNTIME_HARDENING.md`.

### S6 — P1 — Ghidra loader validation gaps — FIXED (Gate D, all 8)

- All eight gaps fixed, none accepted-as-is, each with a fail-before/
  pass-after test: (1) end alignment (`skippedInvalidRange` pre-clamp,
  `skippedUnalignedEnd` post-clamp — with a correction to Gate A's wording:
  the old loader did not "skip" inverted rows, the shared clamp repaired
  them to `[start, start+4)` and promoted them); (2) overlaps (post-loop
  sweep over the new slice only, merge-order sorting, first-wins,
  adjacency not overlap, `mapStarts` rebuilt); (3) decode check, strong
  form (every word must satisfy the rabbitizer R5900 predicate,
  `uniqueId != RABBITIZER_INSTR_ID_r5900_INVALID` = 768, probe-measured;
  plain `cpu_INVALID` 0 is the wrong constant); (4) delay-slot starts
  (`skippedDelaySlot`, no membership exemption); (5) malformed accounting
  (`skippedMalformed` + warnings, old silent `catch(...)` gone);
  (6) header validation (first line matched case-insensitively
  field-by-field to `{name, start, end, size}`, exactly 4 fields;
  non-match treated as data plus one warning); (7) octal parsing
  (`ParseGhidraNumber`: `0x` → hex via `from_chars` with
  full-consumption/overflow checks, else decimal — `"010"` → 10,
  `"0104"` → 104 = `0x68`); (8) row/file bounds (262,144-row cap,
  4,096-char line cap). New additive API:
  `ElfParser::GhidraMapLoadStats` (`loaded` + 8 skip counters +
  `headerSeen` + `rowLimitExceeded`), reset on every call.
- Gate G re-verified in the current tree: the stats struct fields, all
  skip-counter sites, `ParseGhidraNumber`'s body (trim, `0x`/`0X` → hex,
  digit validation, `from_chars` full consumption), the caps
  (`kMaxGhidraMapRows = 262144`, `kMaxGhidraMapLine = 4096`), and the
  README contract lines (caps, number format, header rule, stats names)
  — all agree with the parser. One nit: the parser also accepts uppercase
  `0X`, which the README does not document; harmless superset, not a
  contradiction.
- LABEL HAZARD REAFFIRMED, not filtered: no `entry_*` filter was added.
  The label test was redesigned (not weakened) because gap 2's name-blind
  overlap rule necessarily rejects the two in-body labels — 8 function
  rows + one non-overlapping gap label at `[0x140,0x144)` survive
  (9 functions), in-body labels overlap-rejected, gap label still promotes
  (hazard re-pinned; the test asserts promotion and end survival).
- Evidence: fail-before 661/672 on the unmodified Phase 19 loader (11 new
  tests failed, each for the predicted reason); post-fix 672/672 both
  modes; five independence proofs (Ghidra absent; Java absent; no CSV;
  rejected CSV poisons nothing; symbol precedence beats bogus map row).
- Remaining limitations (carried): decode check is a plausibility filter
  (address-like data words decode as real instructions, probe-proven, so
  data-in-code rows can still pass; rabbitizer is strict about nonzero
  unused fields); E2E-WITH-MAP GAP — the e2e runner has no map-path
  plumbing (grep-verified), so rejected-CSV behavior is proven at unit
  level only; map-vs-symbol partial overlaps with different starts remain
  downstream merge behavior; the loaded-map set rule (map omits →
  discovery drops) is a standing Phase 19 semantic, now documented and
  pinned by test.

### S7 — P2 — TLB-miss message decimal after `0x` — FIXED (Gate B)

- Fix: private `messageFor()` using `std::ostringstream` with `std::hex`
  (+ `<sstream>`); address value and exception behavior unchanged; no
  formatting framework. Fail-before/pass-after probe: `0x529530880`
  pre-fix → `0x1f900000` post-fix; `-H` trace proved the transitive
  `<iostream>` → `<string>` path pre-fix. Test: "GateB S7: Ps2TlbMissError
  message renders the address in hex" (present in the current tree).

### S8 — P1 — CI omits `e2e_stripped` + `e2e_gs_sprite` — FIXED (Gate F, Linux legs)

- Fix: the e2e steps in `linux-gcc-clang` and `linux-clang-sanitizers`
  now select `^e2e_(basic|branchdelay|vu_macro|stripped|gs_sprite)$`
  under the existing `xvfb-run -a`. Gate G re-verified both regex lines
  in the current workflow. Windows job untouched by documented design (no
  WGL driver on hosted runners). Still 5 jobs, YAML parses, zero
  `continue-on-error` re-verified (Gate G: 0 occurrences).
- Residual risk, stated plainly: local `xvfb-run` passes are
  necessary-but-not-sufficient evidence for CI (CI's Mesa/llvmpipe differs
  from local; gs_sprite pixels are CPU-backend output, so only
  window-context creation depends on Mesa). The first CI run after merge
  is the true verification.
- Header-check CI wiring (Gate B mechanism, Gate F item): steps added
  post-configure on both Linux legs —
  `python3 tools/check_public_headers.py --repo . --deps build/_deps`,
  with `CXX` exported per matrix leg (`g++`/`clang++`) and
  `CXX=clang++` on the sanitizers leg. Gate G verified step placement
  (gcc job: after Configure/Build/Tests/e2e; sanitizers job: after e2e)
  and both invocations in the current workflow. Local validation:
  71/71 PASS under g++ (full layout) and 71/71 PASS in CI-fidelity mode
  without parallel-gs (Gate F); Gate G re-ran 71/71 PASS against the
  dependency cache. Clang leg locally UNVERIFIED (`which clang` empty,
  re-checked by Gate G). MSVC leg NOT wired (script uses GCC/Clang-style
  flags) — adaptation recorded as UNVERIFIED procedure (try clang-cl
  first, else a script patch adding an MSVC flag mode `/std:c++20`,
  `/Zs`). No MSVC result claimed.
- Placement note (observation, not a defect): the workflow steps run
  post-build, not immediately post-configure as Gate B's snippet
  described; the check needs only the post-configure `_deps` layout, so
  placement is functionally equivalent. Locally, the CI invocation
  (`--deps build/_deps`) does not reproduce against the
  override-configured build dir (whose `_deps` holds only `*-build`
  dirs, no `-src` dirs); the local equivalent is
  `--deps ~/workspace/zmbor-deps` (verified 71/71 by Gate G). The CI
  invocation is correct for CI, where FetchContent creates `-src` dirs.

### S9 — P2 (process) — Anonymous CI log access blocked (403) — OPEN

- Unchanged: job-log download returns 403 anonymously. Consequence: all
  MSVC verbatim-diagnostic confirmation must go through the user's GitHub
  connection (procedure in `CROSS_PLATFORM_VERIFICATION.md`). This is the
  single largest open verification item for the publication stack.

### S10 — P2 (process) — Working tree / index is not a publication artifact — RESOLVED BY PROCEDURE

- The forward-stack procedure (§2.4 of the Gate A report) was executed by
  Gates B–F and re-executed by Gate G: fresh anonymous clone at 50b7830 →
  `git apply --check` clean → apply → recursive tree comparison vs the
  working tree showing zero differences except the excluded HEAD-only
  files and README (see §3). The working tree's index was never treated
  as a publication artifact. Gate G's own validation is recorded in §3.

### S11 — P2 — README regression risk — RESOLVED BY EXCLUSION

- The forward patch deliberately excludes `README.md`, so a patched clone
  keeps HEAD's newer version. Gate G verified this explicitly: the
  excluded paths are untouched by the patch (`git diff HEAD` empty,
  status clean on those paths in the validation clone), and the only
  content difference between the validation clone and the working tree is
  the README itself (working tree's older version vs HEAD's newer one) —
  which is the intended outcome, not a drift.

### S12 — P2 — IOP standalone sets no C++ standard — FIXED, declarational (Gate F)

- Framing correction to Gate A: HEAD's `ps2xIOP/CMakeLists.txt` had 0
  `CMAKE_CXX_STANDARD` matches, but the effective standard was already
  `-std=gnu++20` via `target_compile_features(ps2_iop PUBLIC cxx_std_20)`.
  The defect was the missing declaration, not a mismatch.
- Fix: `set(CMAKE_CXX_STANDARD 20)` + `set(CMAKE_CXX_STANDARD_REQUIRED ON)`
  after `project()`, with a Gate F comment stating the effective flag is
  unchanged (extensions stay enabled, still `-std=gnu++20`). Gate G
  re-read the declaration and comment in the current tree.
- Evidence: `flags.make` in both modes still shows `-std=gnu++20` plus a
  verbose TU recompile; standalone suites 4/4 both modes; main-tree
  `libps2_iop.a` refreshed in both builds.

### S13 — P2 (verification item) — Generated-code include path for `ps2_stubs.h` — KEEP (Gate C)

- Evidence-backed KEEP: narrowing the `ps2_runtime` PUBLIC export of
  `src/lib/Kernel` to PRIVATE **breaks the build** —
  `ps2xTest/src/pad_input_tests.cpp:4` has bare `#include "Stubs/Pad.h"`,
  and multiple `ps2_test_lib` TUs use bare `Stubs/...` includes resolving
  only via that export (its own PRIVATE dir covers `Kernel/Stubs/...`
  but not bare `Stubs/...`). The change was reverted byte-identically;
  `ps2xRuntime/CMakeLists.txt` is unmodified by Gate C in this respect
  (PUBLIC export still present). Generated code itself needs only the
  public include dir (probe-verified). Any future narrowing must first
  move those test includes. Gate G notes this as a standing constraint,
  not a defect: the export stays until the test includes move.

### S14 — P2 (open audit) — Phase 15 §4.1 exception matrix — AUDITED, ONE DEFECT FIXED (Gate C)

- Fixed: all five `Store*` TLB-miss catch blocks reported TLBL (ExcCode 2);
  per the MIPS table, store misses must report TLBS (3). Added
  `EXCEPTION_TLB_STORE = 0x03` (`ps2_runtime.h:52`); `SignalException`
  still selects the shared refill vector `0x80000000` for it. Gate G
  re-verified: the enum, the refill-vector selection including
  `EXCEPTION_TLB_STORE`, 5 Store sites using TLBS, 5 Load sites using
  REFILL, loads untouched. New test: store miss → Cause 3, BadVAddr,
  refill vector; mutant check fails exactly that test; re-applied passes.
- New tests also: terminal-branch overwrite through the scheduler (branch
  target never runs; dormant context pc = vector, not the branch target);
  ERET emitter semantics (ERL → ErrorEPC + clear ERL, else EPC + clear
  EXL, LL cleared, block stops).
- Audited-correct (existing tests, green): alignment per width ×
  direction, vector selection (general/boot/refill/BEV precedence),
  BadVAddr/EPC/Cause/BD/EXL, CACHE encodings, BREAK (9)/TRAP (13), TLB
  load miss. Full case in `RECOMPILER_RUNTIME_HARDENING.md`.
- Explicit limitations (unchanged): misaligned LQ/SQ mask low 4 bits
  instead of raising AdEL/AdES (pinned model choice); KUSEG
  identity-mapped (documented model choice); TLB KSEG2/3 only; I-cache
  invalidate approximate; no registered vector handler exists in a
  generated program (thread goes dormant — designed path).

## 2. Gate G per-file review of the forward stack (36 files)

**Method.** For each of the 36 files in
`zmbor-forward-50b7830-gateF.patch`, Gate G checked: (a) the forward-diff
size against what the owning gate claimed; (b) the contract-critical
content by direct read of the current tree (contract comments, the
changed code, and the tests that pin it); (c) cross-file consistency
(comments promising behavior the code no longer has, README-vs-parser
agreement, workflow step paths vs the post-configure layout, header
include lists vs the check script's rules). Depth is stated honestly:
contract-critical files were read in full; large carried-forward files
(the Phase 18 adapter, the test files) were reviewed by diffstat plus
targeted spot checks plus the owning gates' test evidence — Gate G did
not perform a fresh line-by-line re-audit of 1,033 lines of adapter code
or 1,203 lines of recompiler tests, and does not claim to have done so.

**Verdict: all 36 files CONSISTENT. No blocking finding. No tree change
by Gate G.** The per-file notes below are the complete record; anything
not listed was checked by diffstat + owning-gate evidence with no anomaly.

| File | Forward delta | Review note |
|---|---|---|
| `.github/workflows/build.yml` | +27 | E2E regex includes `stripped\|gs_sprite` on both Linux legs; header-check steps present with `CXX` per leg; 0 `continue-on-error`; 5 jobs. Placement post-build (see S8 note). |
| `CMakeLists.txt` (root) | +25 | Sccache launcher after `project()` (line 20), before first `add_subdirectory()` (line 155). Matches Phase 20 claim. |
| `docs/engineering/sccache.md` | +195 (new) | Phase 20 doc; numbers agree with the ledger (see `BUILD_PERFORMANCE_EVIDENCE.md`). |
| `ps2xAnalyzer/include/ps2recomp/elf_analyzer.h` | +1 | `<cstddef>` (Gate B). Verified in tree. |
| `ps2xIOP/CMakeLists.txt` | +12 | S12 declaration + comment (Gate F). Verified in tree. |
| `ps2xIOP/include/ps2x/iop/iop_subsystem.h` | +1 | `<cstddef>` (Gate B). Verified in tree. |
| `ps2xRecomp/include/ps2recomp/control_flow_utils.h` | +1 | `#include "ps2recomp/types.h"` (Gate B). Verified; no cycle. |
| `ps2xRecomp/include/ps2recomp/elf_parser.h` | +24 | Gate D `GhidraMapLoadStats` struct + accessor + member. Verified field-for-field against the loader and README. |
| `ps2xRecomp/include/ps2recomp/ps2_recompiler.h` | +1 | `<cstddef>` (Gate B). Verified in tree. |
| `ps2xRecomp/src/lib/elf_parser.cpp` | +523/−(small) | Gate C SLTIU arm (re-read, correct) + Gate D loader (stats sites, `ParseGhidraNumber`, caps, overlap sweep, delay-slot check — spot-verified). Jump-table bound sites untouched. |
| `ps2xRecomp/tools/ghidra/ExportPS2FunctionsHeadless.java` | +102 (new) | Functions-only via `FunctionManager.getFunctions`; header comment documents the label-port rejection (0/19). No label collection. |
| `ps2xRecomp/tools/ghidra/README-GHIDRA-ORACLE.md` | +181 (new) | CSV contract agrees with the parser (caps, number format, header rule, stats names, delay-slot rationale, label-hazard re-pin). Nit: parser also accepts uppercase `0X`, undocumented. |
| `ps2xRuntime/CMakeLists.txt` | +73 | Phase 18 PGS block (gated on the option, default OFF) + Phase 20 hook removal. Option line verified OFF by default; FetchContent gated on the option. |
| `ps2xRuntime/include/ps2_log.h` | +1 | `<cstddef>` (Gate B). API is tracing-only (`log_entry`/`log_exit`) — relevant to the lastBackendError decision (see §4). |
| `ps2xRuntime/include/ps2_runtime.h` | +2 | `<cstddef>` (Gate B) + `EXCEPTION_TLB_STORE = 0x03` (Gate C). Verified. |
| `ps2xRuntime/include/ps2_stubs.h` | +8 | TODO/TODO_NAMED declarations moved in; only a comment mentions the private header now. Verified. |
| `ps2xRuntime/include/runtime/gs/gs_backend.h` | +30 | Stage 0 contract as amended by Gate E: history replay, raw skip, no snapshot copying, transfer-idle, sticky LastError, integration-gap sentence. Read in full; consistent (see §4 wording note). |
| `ps2xRuntime/include/runtime/gs/gs_cpu_backend.h` | +1 | `<cstddef>` (Gate B). Verified in tree. |
| `ps2xRuntime/include/runtime/gs/gs_frontend.h` | +21 | `m_loadClutHistory` member + Gate E comments; last-load fields gone. Verified; no stale member references remain. |
| `ps2xRuntime/include/runtime/gs/gs_parallel_backend.h` | +157 (new) | Phase 18 adapter interface; default-OFF; E2 FAILED status unchanged. |
| `ps2xRuntime/include/runtime/ps2_ipu.h` | +1 | `<cstddef>` (Gate B). Verified in tree. |
| `ps2xRuntime/include/runtime/ps2_memory.h` | +15 | `<string>` (S1) + `<sstream>`/hex `messageFor` (S7). Verified include list and constructor. |
| `ps2xRuntime/include/types.h` | +1 | `<cstddef>` (Gate B). Verified in tree. |
| `ps2xRuntime/src/lib/Kernel/Stubs/Unimplemented.h` | +9 | Shim (Gate B). Verified content. |
| `ps2xRuntime/src/lib/gs/gs_frontend.cpp` | +21/−12 | Gate E only: reset clears history, swap replays history, delivery records history. Forward hunk re-read in full; matches the tree and the contract. |
| `ps2xRuntime/src/lib/gs/gs_parallel_backend.cpp` | +1033 (new) | Phase 18 adapter; Present-on-not-ready returns empty frame and preserves the original error (sets error only if none recorded); `Initialize` clears `m_lastError` (consistent with the sticky rule — `Reset()` does not clear). Spot-verified. |
| `ps2xRuntime/src/lib/ps2_runtime.cpp` | +12 | Gate C TLBS sites only (5 Store sites). Verified against the enum and vector selection. No other behavior change in the forward diff. |
| `ps2xTest/CMakeLists.txt` | +1 | PGS test registration (Phase 18). |
| `ps2xTest/src/gs_parallel_backend_tests.cpp` | +301 (new) | Phase 18 adapter tests. |
| `ps2xTest/src/main.cpp` | +2 | PGS test registration (Phase 18). |
| `ps2xTest/src/ps2_gs_tests.cpp` | +526 | Gate E S4a–S4d + CSM2 swap tests + guards/pins (names verified). |
| `ps2xTest/src/ps2_memory_tests.cpp` | +22 | Gate B S7 hex test (name verified) + Phase 15 TLB tests. |
| `ps2xTest/src/ps2_recompiler_tests.cpp` | +1203 | Gate C S3 tests + Gate D gap 1–8 + independence tests + label test (names/comments verified). |
| `ps2xTest/src/ps2_runtime_expansion_tests.cpp` | +257 | Gate C S5 tests (4, names verified). |
| `ps2xTest/src/ps2_runtime_kernel_tests.cpp` | +36 | Gate C S14 TLBS test (name verified) + Phase 15 tests. |
| `tools/check_public_headers.py` | +208 (new) | Gate B mechanism; both dep layouts + PGS define verified in code; Gate G re-ran 71/71 PASS. |

## 3. Gate G forward-stack validation (re-executed, not inherited)

- Fresh anonymous clone of `https://github.com/m7kor/Zmbor`, checked out
  at exactly `50b78304b627236937887737d9ed4b5eebebf1b8` (verified via
  `git rev-parse HEAD`).
- `git apply --check` on `zmbor-forward-50b7830-gateF.patch`: clean.
  Applied clean.
- Recursive tree comparison (`diff -r`, excluding `.git`, `mc0`,
  `mc1`, `__pycache__`, `*.pyc`) between the validation clone and the
  working tree: **zero content differences except the intended ones** —
  (a) `README.md` differs (working tree's older version vs HEAD's newer
  version — the forward patch deliberately excludes README, so the
  patched clone correctly keeps HEAD's); (b) HEAD-only files present
  only in the validation clone: `docs/architecture/NEXT_GENERATION_RECOMPILER_DESIGN.md`,
  `docs/research/`, `research/poc1`, `research/poc2`, `research/poc3`.
- Negative checks verified explicitly in the validation clone:
  `git diff HEAD --stat` on `README.md`, `docs/research`,
  `docs/architecture/NEXT_GENERATION_RECOMPILER_DESIGN.md`,
  `research/` is empty, and `git status` on those paths is clean —
  the patch touches none of them.
- Test gates on the validated tree: see the final report (Release and
  ASan+UBSan suites, full CTest both modes, IOP standalone both modes,
  e2e_gs_sprite and e2e_stripped both modes). The validation build used
  the same dependency-cache overrides as the main builds, with sccache
  on PATH reusing the Phase 20 warm cache.

## 4. Gate G observations (not defects, recorded so they are not re-litigated)

1. **Contract wording (gs_backend.h, ERRORS section).** The contract says
   "the frontend surfaces the string through its existing debug/error
   path" and, two sentences later, "No production code currently polls
   that channel outside tests and tooling; that is an integration gap,
   not a channel defect." These are compatible — the accessor
   `GS::lastBackendError()` IS the debug path used by tests/tooling —
   but the first sentence reads more strongly than the second allows.
   Gate G judges this a wording looseness, not a contradiction, and did
   not rewrite the contract for it. If the sentence is ever tightened,
   it should name the accessor explicitly.
2. **README `0X` nit.** The Ghidra README documents `0x`-prefixed hex;
   the parser also accepts uppercase `0X`. Harmless superset; no change.
3. **Header-check step placement.** The workflow runs the check
   post-build, not immediately post-configure. Functionally equivalent
   (it needs only the post-configure `_deps` layout). No change.
4. **Local vs CI `_deps` layout.** See S8 note above. No change.
5. **lastBackendError() consumer — DECISION (b), recorded as a remaining
   limitation.** Gate G investigated: no production consumer exists in
   any tree location searched (runtime, recompiler, analyzer, IOP,
   tests-excluded, studio — zero hits outside tests and the frontend
   accessor); `gs_frontend.cpp` includes `ps2_log.h` but has zero log
   call sites; `ps2_log.h` offers only `log_entry`/`log_exit` tracing,
   no error channel; the runtime's existing diagnostic path is
   `std::cerr` in `ps2_runtime.cpp` init paths, which is not a GS
   error channel and would make library logging policy, not a minimal
   fix. A genuine consumer needs either a studio host-loop poll point
   (studio frame loop reading `GS::lastBackendError()` and surfacing it
   in the debug UI — studio work, GUI-gated, not stabilization) or a
   new backend-error callback API registered at GS construction (new
   architecture, needs design review). No production failure has ever
   been observed lost through this channel (the CPU backend never fails
   this way; PGS failures are init-time and observable via
   `lastBackendError()` immediately after attach). Gate G therefore does
   NOT manufacture a consumer in a review gate. Precise future work:
   (i) studio poll point, or (ii) error-callback API design + tests.
   This decision is also recorded in `RECOMPILER_RUNTIME_HARDENING.md`
   and the ledger. No diagnostics subsystem was built.

## 5. What Gate G did NOT do (scope boundary)

- No tree change, no new patches: the Gate F forward patch stands as the
  publication stack. (Manufacturing a patch for a review-only gate would
  have been scope expansion.)
- No SPU2/VU/SIF/COP0/GS behavior change, no tolerance or golden change,
  no Stage 3 work, no `components.json` change, no sccache file change,
  no push/PR/merge, no credential request.
- No MSVC/Clang execution (no toolchains, no authorized CI access). Those
  outcomes remain OPEN and cap the final verdict (see the final report).

## 6. Lineage restated (for the final report)

- Historical baseline: `350ca1f3b2cd2aede0d5ec8491b36d2c8a552141`
  (2026-10-28). Cumulative record at Gate F:
  `zmbor-cumulative-gateF-verification-tooling.patch`, 19,970 lines,
  78 files vs baseline.
- Publication base: `50b78304b627236937887737d9ed4b5eebebf1b8`.
  Publication stack: `zmbor-forward-50b7830-gateF.patch`, 5,645 lines,
  36 files, 0 deleted files — the unpublished delta Phase 18 → 19 → 20
  → Gates B → C → D → E → F. It supersedes the Gate E forward patch
  (not deleted).
- Next: publication via the user's GitHub connection (review branch,
  never main, never force-push), then the MSVC/Clang publication-run
  outcomes, then the queued GS hardware-truth arbitration (triangle edge
  + bilinear filtering) and, only when a real title runs graphically,
  the audio sink.
