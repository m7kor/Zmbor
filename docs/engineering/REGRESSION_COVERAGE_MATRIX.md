# Zmbor Stabilization — Regression Coverage Matrix (Gate G, final integration review)

**Date:** 2026-11-04 (Asia/Riyadh). **Scope:** every stabilization
gate's tests mapped to the defect or contract they pin, with
fail-before/pass-after evidence where it exists, and the explicit gaps
that remain. This document is a review record staged outside the repo
tree; it is not a tree change and does not alter any component status.

**How to read this matrix.** "Fail-before" means the test was run
against the pre-fix code and failed for the predicted reason — the
evidence that the test actually guards the defect rather than passing
vacuously. "Passes pre- and post-fix as designed" means the test pins a
contract that the fix must not break; it is a guard, not a defect
detector, and is labeled as such. Counts are `ps2x_tests` totals
(Release and ASan+UBSan agreed at every gate).

## 1. Suite totals by gate (regression floor)

| Gate | Total | Delta vs prior gate | CTest (`xvfb-run -a`) | IOP standalone |
|---|---|---|---|---|
| Pre-stabilization (Phase 20) | 650 | — | 56 total (55 passed / 1 skipped z3 / 0 failed) | 4/4 both modes |
| B (Windows headers) | 651 | +1 (S7) | 56 total both modes | 4/4 both modes (rebuilt; `iop_subsystem.h` touched) |
| C (runtime/recompiler) | 661 | +10 (S3 3, S5 4, S14 3) | 56 total both modes | 4/4 both modes |
| D (Ghidra loader) | 672 | +11 (S6 gaps + independence) | 56 total both modes | 4/4 both modes (prebuilt Gate C binaries; Gate D touched no IOP file) |
| E (GS contracts) | 683 | +11 (5 S4 sequences + 6 guards/pins) | 56 total both modes | 4/4 both modes (prebuilt Gate C binaries; Gate E touches no IOP file) |
| F (verification tooling) | 683 | +0 (wiring only, no new tests) | 56 total both modes | 4/4 both modes (rebuilt with S12) |
| G (this review) | 683 | +0 (review only, no tree change) | 56 total both modes on the validated tree | 4/4 both modes from the validated tree |

No gate ever reduced the total, and no gate ever weakened a test,
assertion, tolerance, or diagnostic to obtain a pass. Where a test
expectation was corrected during a gate, it is recorded below as a
correction in the open, with the reason.

## 2. Gate B — Windows correctness (S1, S2, S7)

| Test / mechanism | Defect or contract pinned | Fail-before evidence |
|---|---|---|
| `tools/check_public_headers.py` (mechanism, not a unit test): per-header standalone compilation + direct-include audit | S1 missing `<string>` (caught by the audit under GCC where standalone compilation passes via transitive includes); S2 missing `<cstddef>` / missing include / private-dir include | Pre-fix GCC: 71 headers, PASS=60 FAIL=11 (mode-1 failures exactly the S2 three; mode-2 flags `ps2_memory.h` + 8 `<cstddef>` headers). Post-fix: PASS=71 FAIL=0 in both dep layouts. Gate G re-ran: 71/71 PASS under g++ against the dependency cache. |
| "GateB S7: Ps2TlbMissError message renders the address in hex" | S7 decimal-after-`0x` rendering | Probe: `0x529530880` pre-fix → `0x1f900000` post-fix; `-H` trace proved the transitive `<iostream>` → `<string>` path pre-fix. Preserves address value and exception behavior. |
| Full suite + CTest + e2e (generated-code compilation) | S2 `ps2_stubs.h` fix does not break generated code | 651/651 both modes; CTest 56 both modes incl. all 5 e2e. |
| CI wiring (Gate F, mechanism) | S1/S2 regression mechanism runs in CI | Steps on both Linux legs; local validation 71/71 PASS (g++, full layout) and 71/71 PASS (CI-fidelity mode without parallel-gs). Clang leg locally UNVERIFIED; MSVC leg NOT wired (see §5). |

## 3. Gate C — Runtime / recompiler integrity (S3, S5, S13, S14)

| Test | Defect or contract pinned | Fail-before evidence |
|---|---|---|
| S3 SLTIU tests (3, synthetic ELF, Phase 14 JALR harness pattern) | SLTIU false-proof: negative immediates, `rs` straddling `0xFFFF`/`0x10000`/`0xFFFFFFFF`, SLTI-vs-SLTIU transitions, proof invalidation | Fail-before: 654 total / 653 passed / 1 failed — exactly the negative-immediate test (both −1 and −32768 variants fail). Mutant check (revert to `imm16`) fails exactly that test (653/654). |
| "GateC S5 exception through guest frame runs destructors and keeps vector state" | R1/R2: exception through a generated block with live locals; destructor runs; dormant context holds vector/EPC/Cause/AdEL/BadVAddr/EXL | Passes post-fix (no code change — PROVEN-SAFE verdict; the test pins the contract). |
| "GateC S5 exception from completion callback survives the swallow site" | R2 site 262: onComplete-raised BREAK survives | Passes post-audit (pins the contract). |
| "GateC S5 exception from resume completion survives the swallow site" | R2 site 222: resumeCompletion-raised BREAK via `waitExternal` + cross-thread `ExternalWake` survives | Passes post-audit (pins the contract). |
| "GateC S5 exception from interrupt handler abandons invocation, thread resumes" | Interrupt-path fault: invocation abandoned cleanly, continuation runs exactly once | Passes post-audit (pins the contract). |
| "GateC S14 store TLB miss raises TLBS, not TLBL" | Store TLB miss reported TLBL (2), must report TLBS (3); BadVAddr; refill vector | Mutant check fails exactly this test (660/661); re-applied, 661/661. |
| Terminal-branch overwrite test (S14) | Branch target never runs after abort; dormant context pc = vector, not the branch target | New test with the fix (abort mechanism from Phase 15). |
| ERET emitter test (S14) | ERL → ErrorEPC + clear ERL, else EPC + clear EXL, LL cleared, block stops | New test with the fix. |
| S13 (no new test) | Generated-code include path; PUBLIC `src/lib/Kernel` export KEEP | Evidence is a failed build, not a test: narrowing the export breaks `pad_input_tests.cpp:4` bare `Stubs/Pad.h` and other bare `Stubs/...` test includes. Change reverted byte-identically. |
| S14 audited-correct rows (alignment, vectors, BadVAddr/EPC/Cause/BD/EXL, CACHE, BREAK/TRAP, TLB load miss) | Phase 15 §4.1 matrix | Existing tests, green; no new tests needed. See `RECOMPILER_RUNTIME_HARDENING.md` §4. |

**Test-authoring lesson (Gate C, recorded for future gates).**
`TestCase::Run` stores callbacks for later execution — test lambdas
must NEVER capture registration-function locals by reference. Gate C's
first draft had dangling captures, fixed before the first build;
shared helpers live at file scope.

## 4. Gate D — Ghidra loader validation (S6)

| Test | Defect or contract pinned | Fail-before evidence |
|---|---|---|
| Gap 1 — unaligned end (0x44 row's end made unaligned) | End alignment: pre-clamp `requestedEnd <= start` → `skippedInvalidRange`; post-clamp `(end & 0x3) != 0` → `skippedUnalignedEnd` | Part of the 11 fail-before set (661/672 on the unmodified Phase 19 loader). |
| Gap 2 — overlapping ranges (full 8-row set + one overlap row) | Overlaps: first-wins, adjacency not overlap, `mapStarts` rebuilt; size stays 8, `skippedOverlapping` 1 | Part of the 11 fail-before set. |
| Gap 3 — decode check + R5900-class coverage | Strong decode: every word must satisfy the rabbitizer R5900 predicate; row spanning `0x74000000` rejected, paddw/LQ/SQ/COP2 row promotes; `skippedUndecodable` 1 | Part of the 11 fail-before set. |
| Gap 4 — delay-slot start (12-word `.text`) | Predecessor at `start−4` with delay slot → `skippedDelaySlot`; beq delay-slot row rejected, following row promotes | Part of the 11 fail-before set. |
| Gap 5 — malformed-row accounting (4 malformed rows) | `skippedMalformed` + warnings; trailing comma tolerated, non-empty 5th field counted; old silent `catch (...)` gone | Part of the 11 fail-before set (incl. the EXTRA-5th-field row promoting pre-fix). |
| Gap 6 — headerless file | First line matched to `{name, start, end, size}`; non-match treated as data + one warning; both rows promote, `headerSeen` false | Part of the 11 fail-before set (headerless first row dropped with no warning pre-fix). |
| Gap 7 — octal parsing (`.text` at `0x40`) | `ParseGhidraNumber`: `0x` → hex, else decimal; asserts `0x68` present, `0x44` absent, hex row works | Part of the 11 fail-before set. |
| Gap 8 — row-count bound (262,144 non-exec fillers + trailing TAIL) | 262,144-row cap: warn + break, keep accepted rows; load false, TAIL absent, `rowLimitExceeded` true, `skippedNonExecutable` 262144 | Part of the 11 fail-before set. |
| Gap 8 — line-length bound (5,000-char line) | 4,096-char line cap → malformed; next row promotes | Part of the 11 fail-before set. |
| Independence — fully rejected CSV must not poison the function set | Inverted + unaligned + misaligned + non-exec rows → load false, discovery entry + `0x28` survive, all four invalid starts absent, one count per bucket | New test with the fix. |
| Independence — bogus map row must never override a verified ELF symbol | `merged_fn` symbol `[0x20,0x34)` beats BOGUS map row `[0x20,0x28)` (name `merged_fn`, end `0x34`) | Passes pre- and post-fix by design (guard, not detector). |
| Label test (redesigned, not weakened) | Label hazard re-pinned: 8 function rows + non-overlapping gap label at `[0x140,0x144)` survive (9 functions); in-body labels overlap-rejected; gap label still promotes | Fail-before: label test yielded 11 vs expected 9 on the old loader (part of the 11 fail-before set). |
| LOAD test (Phase 19, unchanged) | Verbatim POC 1 rows through the real loader → exactly 8 functions with CSV names/ranges | Passed pre-fix as designed; unaffected by the Gate D fixture fix (asserts names/ranges, not words). |
| Lowercase-header merge + symbol guard | Header matching is case-insensitive; merge precedence symbols > map > extras > discovery | Passed pre-fix as designed. |

**Corrections made in the open (Gate D, not weakenings).**
(a) A Gate D delay-slot test expectation was wrong, not the loader: it
asserted discovery of `0x100020` must survive, contradicting the
documented loaded-map set rule (discovered starts admitted only if in
`m_ghidraMapStarts`); it failed pre-fix too; corrected to assert the
set-rule outcome with a citing comment. (b) Fixture realism fix: the
Phase 19 oracle tile put 4 real function starts after `jr` words (i.e.,
in delay slots); words at indices 16/44/54/68 are now `nop` with a Gate D
comment. (c) Pre-fix row-cap "anomaly" explained by the same set rule:
uncapped, TAIL loaded and the set rule dropped the discovery entry;
capped, the load fails and discovery survives.

## 5. Gate E — GS contracts (S4, §5.2)

| Test | Defect or contract pinned | Fail-before evidence |
|---|---|---|
| "GS swap replays CLUT history past a trailing CLD=0 no-op (Gate E S4a)" | CLD=1 load → CLD=0 write → swap (was: palette zeros) | FAIL pre-fix (palette zeros); PASS post-fix, pixel-compared before vs after swap. |
| "GS swap replays CLD slot history for a trailing CLD=4/CBP=0 load (Gate E S4b)" | CLD=2 slot 128 → CLD=4/CBP=0 → swap (was: fresh slot 0 skips load) | FAIL pre-fix; PASS post-fix. |
| "GS swap replays multiple CBP/CSA slices, CSM1/CT16 8-bit (Gate E S4c)" | Two CSM1/CT16 8-bit slices (csa 0/16; was: first slice zeros) | FAIL pre-fix; PASS post-fix. |
| "GS swap replays CSM1-suffix partial loads (Gate E S4d)" | CSM1-suffix partial loads ([16,256) slice; was: entries [0,16) zeros) | FAIL pre-fix; PASS post-fix. |
| "GS swap replays CSM2 multi-slice loads, 4-bit (Gate E CSM2)" | Two CSM2/CT32 4-bit slices (csa 0/1); closes the Stage 1 CSM1-only fixture gap | FAIL pre-fix; PASS post-fix. |
| TEXFLUSH guard | Snapshot semantics pinned: replay reads current VRAM; TEXFLUSH does not touch `m_clut` | Passes pre- and post-fix as designed. |
| Reset rule | History clears on `reset()`; a naive no-clear implementation fails this test | Passes pre- and post-fix as designed (designed to fail a no-clear implementation). |
| Raw-mode parity | Raw writes never recorded | Passes pre- and post-fix as designed. |
| §5.2 pins (3) | LastError sticky lifetime; failed Present never serves stale pixels; init failure observable via `lastBackendError()` immediately after attach | Passes pre- and post-fix as designed (audited-correct paths). |
| `e2e_gs_sprite` (exact full-frame, tolerance 0) | No CLUT change breaks the GS golden | Passed both modes at Gate E (and F, G). A CLUT regression here is a failed gate, not a tolerance question. |

**Test bugs fixed in the open (Gate E, not implementation bugs).**
Missing TEXFLUSH before a mid-test texel rewrite; raw byte write to a
nibble address. Both failed on pre-swap assertions too, never
implicating the implementation.

**Gate E environment incident (recorded, not hidden).** The first two
main ASan runs crashed deterministically in `PS2RomDevice::configure`
because the coordinator edited `gs_frontend.h` while the ASan build
ran, mixing two `GS` class layouts. Fresh-build bisects proved it
(Gate D tree 672/672, product-only 672/672, full Gate E 683/683); a
clean rebuild passed 683/683. Lesson: never edit a header while a
build runs. Gate G edited nothing in the tree.

## 6. Gate F — Verification / performance tooling (S8, S12)

| Item | Mechanism / evidence | Fail-before evidence |
|---|---|---|
| S8 e2e CI selection | Workflow e2e steps now select `^e2e_(basic\|branchdelay\|vu_macro\|stripped\|gs_sprite)$` on both Linux legs under `xvfb-run -a` | No unit test — the evidence is the workflow diff + local `xvfb-run` passes both modes. CI-run evidence is UNVERIFIED until the first publication CI run (see §7). |
| Header-check CI wiring | Steps post-configure on both Linux legs (`--deps build/_deps`, `CXX` per leg) | Local validation 71/71 PASS (g++, full layout) and 71/71 PASS (CI-fidelity mode without parallel-gs). Clang leg locally UNVERIFIED; MSVC leg NOT wired. |
| S12 IOP standard declaration | `set(CMAKE_CXX_STANDARD 20)` + `REQUIRED ON` after `project()` | No unit test — the evidence is `flags.make` still showing `-std=gnu++20` in both modes + a verbose TU recompile + standalone suites 4/4 both modes. The defect was the missing declaration, not a mismatch. |
| ps2autotests baseline reproduction | EE 53/55, 8202/8250 lines; IOP 23/23, 1306/1306 lines; 0 crashes/timeouts/recompile failures; no `.expected` modified | Zero delta vs the historical baseline. Known failures `cpu_ee_fpu_sqrt` 59/66 and `vu_lower_efu` 182/223 remain open, unfixed, unskipped. Build-type caveat: RelWithDebInfo, not Release (see §7). |
| Gate E follow-through (`lastBackendError()` consumer) | ASSESSED, NOT WIRED: only test consumers exist; `gs_frontend.cpp` includes `ps2_log.h` but has zero log call sites; `ps2_log.h` offers only tracing, no error channel | No test — the evidence is the grep + API read. Decision carried to Gate G (see §7). |

## 7. Explicit gaps (not coverage — read before claiming completeness)

1. **E2E-WITH-MAP GAP (Gate D).** The e2e runner has no map-path
   plumbing (grep-verified). "Stripped e2e with a rejected CSV present"
   is proven at unit level only, never claimed as an e2e run. Closing
   it needs e2e-runner plumbing, not another unit test.
2. **Decode check is a plausibility filter (Gate D).** Address-like
   data words decode as real instructions (probe-proven), so
   data-in-code rows can still pass. Rabbitizer is strict about nonzero
   unused fields. A stronger check (e.g., control-flow plausibility)
   is future work.
3. **Map-vs-symbol partial overlaps with different starts (Gate D).**
   Remain downstream merge behavior, unchanged by Gate D (stated scope
   boundary).
4. **Loaded-map set rule (Phase 19, pinned by Gate D).** Map omits →
   discovery drops. A standing semantic consequence, documented in the
   README and pinned by test. Changing it is a merge-precedence
   decision for a later gate, not a bug fix.
5. **MSVC + Clang legs (Gates B–F, carried to publication).** No local
   results exist; CI results do not exist yet for this stack. The
   header-check MSVC adaptation (clang-cl first, else `/std:c++20` +
   `/Zs`) is an UNVERIFIED procedure.
6. **CI-run evidence for Gate F wiring (S8, header check).** Local
   `xvfb-run` passes are necessary but not sufficient: CI's
   Mesa/llvmpipe differs from local, and a loaded shared runner could
   flake ASan `gs_sprite` (see §8). The first CI run after merge is the
   true verification for both wirings.
7. **ps2autotests build-type caveat (Gate F).** Reproduction used
   RelWithDebInfo (first Release build abandoned: LTO links too slow,
   9/55 after ~11 min). `-DNDEBUG` present in both, generated sources
   `-O0` either way, exact text output — but a Release-built
   reproduction would remove even that caveat.
8. **`lastBackendError()` has no production consumer (Gates E–G).**
   Decision (b) in `RECOMPILER_RUNTIME_HARDENING.md` §5: remaining
   limitation with precise future work (studio poll point, or
   error-callback API design + tests).
9. **PGS `SnapshotVram` stale-mirror fallback has no error signal
   (Gate E, documented not changed).** CPU backend never fails this
   way; forcing a Vulkan-device throw is not feasible in a unit test.
10. **PGS `ClearFramebuffer` / `ConsumeLocalToHostBytes` unimplemented
    (Phase 18, unchanged).** Return false/0; no fixture uses them.
11. **GS hardware-truth gaps (Phase 18 E2, unchanged).** Triangle edge
    fill convention and bilinear interpolation correctness vs real
    hardware are UNKNOWN. The adapter stays default-OFF experimental;
    Stage 3 is NOT reconsidered without hardware evidence. No test in
    this matrix covers hardware edge/interpolation behavior, because
    no such evidence exists in-tree.
12. **ps2autotests known failures (hardware-probe-blocked).**
    `cpu_ee_fpu_sqrt` (RSQRT.S last bits) and `vu_lower_efu` (EFU last
    bits) require a physical PS2; none is available. They are open,
    unfixed, and unskipped — never silently skipped.
13. **No future-noexcept guard (Gate C caveat).** No test guards the
    scheduler throw→catch chain against a future noexcept addition.
14. **S13 standing constraint.** The `ps2_runtime` PUBLIC
    `src/lib/Kernel` export stays until the bare `Stubs/...` test
    includes move. Any future narrowing attempt must first move them.

## 8. Timing / flake notes that affect how these tests read in CI

- ASan `gs_sprite`: ~26.5 s standalone (Gate F) / 25.91 s (main-agent
  ASan CTest re-run at Gate F) vs the 60 s `run_e2e.cmake` timeout —
  headroom unloaded. Under concurrent load (Gate F: `--parallel 4`
  hwtest build on a 4-core VM), it hit 60.38 s and timed out once;
  full re-run passed 56/56. Classified as contention, not regression;
  timeout unchanged. Watch the first publication CI run for this flake
  on shared runners.
- Full ASan CTest wall time and `ps2x_tests` wall times on this VM
  were not recorded under a measurement protocol at Gate G; do not
  quote them.
- HWTest Release builds are LTO-link-bound (9/55 after ~11 min when
  abandoned); RelWithDebInfo was used for the Gate F reproduction.
