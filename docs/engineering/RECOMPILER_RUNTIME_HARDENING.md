# Zmbor Stabilization — Recompiler / Runtime Hardening (Gate G, final integration review)

**Date:** 2026-11-04 (Asia/Riyadh). **Scope:** the Gate C runtime/
recompiler items (S3, S5, S13, S14) with their final evidence, the
scheduler-transfer safety case, the Phase 15 exception matrix as
audited, and the Gate G decision on the `lastBackendError()` consumer.
This document is a review record staged outside the repo tree; it is
not a tree change and does not alter any component status.

**Evidence vocabulary:** IMPLEMENTED / TESTED / VERIFIED /
HARDWARE-CONFIRMED / UNVERIFIED / UNKNOWN, as defined in
`ROOT_CAUSE_AUDIT.md`. **No hardware claim is made here.** All behavior
below is model behavior of the recompiler and runtime, tested against
synthetic and generated programs — not against physical PS2 hardware.

## 1. S3 — SLTIU sign-extension (FIXED, Gate C)

**The defect.** `ResolveBlockConstant` in `ps2xRecomp/src/lib/elf_parser.cpp`
compared SLTIU against the zero-extended 16-bit field (`imm16`).
MIPS/R5900 SLTIU sign-extends the immediate to 32 bits first, then
compares unsigned. Counterexample: `sltiu $t0, $s0, -1` with
`$s0 = 0x10000` must yield 1 (`0x10000 < 0xFFFFFFFF`); the old code
yielded 0 (`0x10000 < 0xFFFF`). The analyzer's "proven" tier therefore
claimed a value the block did not prove — violating the function's own
soundness direction ("may only fail to resolve, never claim a value the
block does not prove"). Severity P1, not P0: direct 0/1 targets are
rejected by downstream code-section/alignment guards, so exploitation
requires an arithmetic chain into a JR/JALR register.

**The fix.** The SLTIU arm now compares
`*known[rs] < static_cast<uint32_t>(simm)`, mirroring the SLTI arm,
with a comment stating the semantics and the counterexample. Gate G
re-read the fixed arm; it matches this description exactly.

**Evidence.** 3 new tests (synthetic ELF, Phase 14 JALR harness pattern):
negative immediates (−1, −32768), `rs` = 0 and `0xFFFFFFFF`,
SLTI-vs-SLTIU transition cases, proof-invalidation cases. Fail-before:
654 total / 653 passed / 1 failed — exactly the negative-immediate
test. Mutant check: reverting to `imm16` fails exactly that test
(653/654); re-applied, 654/654 at that stage. Neighbours unchanged:
SLTI, ADDIU/ADDI, ORI/ANDI/XORI, LUI. Jump-table bound SLTIU sites
untouched — they are fail-safe (a negative bound yields `numCases`
32768–65535, exceeding the 1000 cap, so the table is rejected; lost
precision, never a false table).

**What this does not prove.** Cross-block constant propagation remains
unproven by design (a documented Phase 14 limitation, not a defect).
JALR through loads/arguments and direct-J tail-call splitting remain
undiscovered by design. The fix closes the false-proof hole inside one
block; it does not extend the prover's reach.

## 2. S5 — Scheduler transfer / C++ object lifetime (PROVEN-SAFE, Gate C)

### 2.1 The mechanism, correctly framed

There is no C `setjmp`/`longjmp` in product code (verified by grep:
`setjmp`/`longjmp` appear only in generated symbol-database string data,
an analyzer classifier name list, and unrelated guest-import stubs).
The Phase 15 mechanism is a plain C++ exception:

```cpp
// ps2xRuntime/include/runtime/ee_scheduler.h:19-22
// This exception is the EE equivalent of a longjmp to the dispatcher. It is
// not an error and must only be caught at EeScheduler::run().
struct EeDispatcherTransfer final { };
```

C++ exceptions run destructors during unwinding. The
"longjmp-skips-destructors" UB class therefore does not apply to this
mechanism as implemented. The brief's original longjmp concern was
reclassified, not confirmed. Throw sites raise COP0 exception state
first (`raiseCop0Exception`), then throw, so the generated block aborts
instead of its terminal branch overwriting the vector PC. All three
catch sites sit inside `EeScheduler::run()` (from line 159): site 222
(resume-completion callback, swallowed, loop continues), site 262
(invocation `onComplete` callback, swallowed, loop continues), site
310–322 (around the guest function call, resets `m_guestExecuting` /
`m_insideInterrupt`, loop continues to redispatch at the vector PC). A
trailing `catch (...)` stops the scheduler (`m_running = false`).

### 2.2 R1 — noexcept boundary → `std::terminate` — PROVEN-SAFE

Full throw→catch chain audit (Gate C): generated code →
`eeCheckpointDue` (noexcept) → `checkpointDue` → `accountCycles` →
`advanceEeTimers` + `advanceIopEeCycles` → `runEeCycles` (noexcept;
`runCycles` is noexcept AND swallows everything in `try/catch(...)`) +
map-find/iteration/arithmetic/atomic-load links. No link on the chain
can throw. The production `catch(...)` rethrow in `run()` is caught by
the gameThread lambda — never a terminate. Gate A had grepped the two
emitter files for `noexcept` and found none; Gate C completed the chain.

### 2.3 R2 — swallow sites losing exception PC/state — PROVEN-SAFE

The transfer carries no state. State lives in the live context the
loop re-reads: site 222 receives the live `activeContext()`; site 262
runs after the completed invocation is popped. All existing `onComplete`
callbacks were audited (IPU, SIF, RPC, System, Thread, EeScheduler) and
write to live parent/host state, never to a dead context.

### 2.4 Adversarial tests (all passing; names verified by Gate G)

1. "GateC S5 exception through guest frame runs destructors and keeps
   vector state" — RAII destructor runs during unwinding; dormant
   thread context holds vector/EPC/Cause/AdEL/BadVAddr/EXL.
2. "GateC S5 exception from completion callback survives the swallow
   site" — onComplete-raised BREAK survives site 262.
3. "GateC S5 exception from resume completion survives the swallow
   site" — resumeCompletion-raised BREAK (via `waitExternal` +
   cross-thread `ExternalWake`) survives site 222.
4. "GateC S5 exception from interrupt handler abandons invocation,
   thread resumes" — true `dispatchIrq` path abandons the invocation
   cleanly; the thread continuation runs exactly once.

### 2.5 Real finding folded into the tests (not a defect)

A function cannot be registered at the KSEG0 vector `0x80000080`
(`registerFunction` writes only the dense table `[0x0, 0x2000000)`).
After an abort, therefore, the thread goes dormant via the designed
missing-function path — the tests assert that designed fallback, not a
vector function. If a future change introduces a sparse vector table,
these tests must be revisited.

### 2.6 Caveat (carried)

PROVEN-SAFE covers existing paths only. A future noexcept-boundary
introduction anywhere on the throw→catch chain, or a new callback that
raises into a dead context, needs re-audit. No standing test guards
the chain against future noexcept additions; that is a review gap for
future work, stated here so it is not mistaken for coverage.

## 3. S13 — Generated-code include path for `ps2_stubs.h` — KEEP (Gate C)

**Question.** Generated TUs emit `#include "ps2_stubs.h"`, and that
header (pre-Gate-B) included private `Stubs/Unimplemented.h`. The e2e
programs passed, so some include path must have covered it — but Gate A
had not verified which one.

**Answer (evidence-backed KEEP).** Narrowing the `ps2_runtime` PUBLIC
export of `src/lib/Kernel` to PRIVATE **breaks the build**:
`ps2xTest/src/pad_input_tests.cpp:4` has bare
`#include "Stubs/Pad.h"`, and multiple `ps2_test_lib` TUs use bare
`Stubs/...` includes resolving only via that export (its own PRIVATE
dir covers `Kernel/Stubs/...` but not bare `Stubs/...`). The change was
reverted byte-identically; `ps2xRuntime/CMakeLists.txt` still carries
the PUBLIC export (`target_include_directories(ps2_runtime PUBLIC
include + src/lib/Kernel)`). Generated code itself needs only the
public include dir (probe-verified). **Standing constraint:** any
future narrowing must first move those test includes. Gate B's
`ps2_stubs.h` fix (declarations moved into the public header,
`Unimplemented.h` now a shim) is independent of this export and stands.

## 4. S14 — Phase 15 §4.1 exception matrix (AUDITED, ONE DEFECT FIXED, Gate C)

### 4.1 The defect fixed: store TLB miss reported TLBL, must report TLBS

All five `Store*` TLB-miss catch blocks reported TLBL (ExcCode 2). Per
the MIPS table, store misses must report TLBS (3). Fix: added
`EXCEPTION_TLB_STORE = 0x03` (`ps2xRuntime/include/ps2_runtime.h:52`);
`SignalException` still selects the shared refill vector `0x80000000`
for it. Gate G re-verified: the enum, the refill-vector selection
(including `EXCEPTION_TLB_STORE`), 5 Store call sites using TLBS, 5
Load sites using REFILL, loads untouched. Test: "GateC S14 store TLB
miss raises TLBS, not TLBL" — store miss → Cause 3, BadVAddr, refill
vector. Mutant check fails exactly that test; re-applied, passes.

### 4.2 Matrix coverage (audited-correct, existing tests green)

| Matrix row | Status | Evidence |
|---|---|---|
| Alignment per width × direction (LH/LW/LD/SH/SW/SD, load + store) | Audited-correct | Existing tests; Phase 15 fast-path routing to checks (misaligned RAM accesses no longer silently succeed) |
| Vector selection (general / boot / refill / BEV precedence) | Audited-correct | Existing tests |
| BadVAddr / EPC / Cause / BD / EXL per fault | Audited-correct | Existing tests |
| CACHE encodings (field 2/3 → RI + abort; valid I/D ops no-op) | Audited-correct | Existing tests; I-cache invalidate documented approximate (see §4.3) |
| BREAK (9) / TRAP (13) termination | Audited-correct | Existing test "Phase15 BREAK and TRAP abort to the exception vector" |
| Delay-slot exception PC | Audited-correct | Existing tests |
| Terminal-branch overwrite protection | Audited-correct + new test | New test: branch target never runs; dormant context pc = vector, not the branch target |
| ERET emitter semantics | New test | ERL → ErrorEPC + clear ERL, else EPC + clear EXL, LL cleared, block stops |
| TLB load miss | Audited-correct | Existing tests ("Phase15 KSEG2 TLB hit translates, TLB miss raises refill"; "Phase15 KSEG2 TLB hit translates, miss throws Ps2TlbMissError") |
| Block abort via `EeDispatcherTransfer` | Audited-correct | Exception state + abort; terminal branch no longer overwrites vector PC (see §2) |
| BadVAddr now set (was unset) | Fixed in Phase 15, audited in Gate C | Existing tests |
| TLB-miss distinguished from generic AdEL/AdES (`Ps2TlbMissError`, derives from `std::runtime_error`) | Fixed in Phase 15, audited in Gate C | Existing tests |

### 4.3 Explicit limitations (unchanged, pinned model choices)

- Misaligned LQ/SQ mask low 4 bits instead of raising AdEL/AdES
  (pre-existing choice, kept deliberately — changing it is not a
  stabilization fix).
- KUSEG stays identity-mapped (documented model choice; full KUSEG
  translation NOT attempted).
- TLB translation covers KSEG2/3 only.
- I-cache invalidate is approximate (exact for D-cache, approximate
  for I-cache).
- No registered vector handler exists in a generated program: after an
  abort the thread goes dormant via the designed missing-function path
  (see §2.5). This is a designed path, not a silent failure — but it
  means generated programs do not yet handle exceptions by running a
  guest handler.

## 5. Gate G decision: the `lastBackendError()` consumer — (b) RECORDED AS REMAINING LIMITATION

**The gap (Gate E, confirmed by Gate F, re-confirmed by Gate G).**
No production code polls `GS::lastBackendError()` outside tests and
tooling. Gate G searched the runtime, recompiler, analyzer, IOP, studio,
and tooling trees: zero production consumers (only the frontend
accessor itself and the contract comment mention it). `gs_frontend.cpp`
includes `ps2_log.h` but has zero log call sites. `ps2_log.h` offers
only `log_entry`/`log_exit` tracing — no error channel. The runtime's
existing diagnostic path is `std::cerr` in `ps2_runtime.cpp` init
paths, which is not a GS error channel.

**Why Gate G does not manufacture a consumer.** A genuine consumer
needs either (i) a studio host-loop poll point — the studio frame loop
reading `GS::lastBackendError()` each frame and surfacing it in the
debug UI (studio work, GUI-gated, not stabilization), or (ii) a new
backend-error callback API registered at GS construction (new
architecture, needs design review). A `std::cerr` print from the GS
frontend would be new library logging policy, not a minimal fix, and
would not reach the debug UI that is the natural consumer. No
production failure has ever been observed lost through this channel:
the CPU backend never fails this way, and PGS failures are init-time
and observable via `lastBackendError()` immediately after attach
(pinned by Gate E test). The channel itself is sound — sticky
lifetime, latch clears on empty frame, init failure observable — and
its contract comment states the gap plainly ("that is an integration
gap, not a channel defect").

**Decision: (b).** Recorded as a remaining limitation with precise
future work: (i) studio poll point, or (ii) error-callback API design
+ tests. No diagnostics subsystem was built in Gate G. No tree change
was made for this item.

**Related, documented-not-changed (Gate E, carried).** PGS
`SnapshotVram` (const) falls back to the last good mirror on
`refreshMirror` throw without setting LastError. The CPU backend never
fails this way, and forcing a Vulkan-device throw is not feasible in a
unit test, so per "fix only what is demonstrably wrong" it is
documented, not blind-changed. PGS `ClearFramebuffer` /
`ConsumeLocalToHostBytes` remain unimplemented (return false/0,
unchanged from Phase 18; no fixture uses them).

## 6. What hardening did NOT change (scope boundary)

- No recompiler pipeline change beyond the SLTIU arm and the Gate D
  loader validation (both in the analyzer, both test-pinned).
- No scheduler rewrite, no exception-mechanism change, no new
  exception type beyond `EXCEPTION_TLB_STORE` (an enum value, not a
  behavior change in itself — the behavior change is the five Store
  sites reporting it).
- No KUSEG translation, no LQ/SQ misalignment exceptions, no I-cache
  model change. Those remain documented model choices for future work
  with hardware evidence, not stabilization fixes.
- No `components.json` status change. No hardware claim.
