# Zmbor Stabilization — Gate A Baseline Report (READ-ONLY)

**Gate:** A — Baseline and isolation. No fixes, no tree edits, no commits, no pushes,
no ledger edits, no credentials used. All findings below are from read-only inspection
(clone, API GETs, `git status`/`git diff`, file reads, and GCC standalone-compile probes
in `/tmp`). Nothing in the working tree was modified by Gate A.

**Report date:** 2026-11-03 (Asia/Riyadh). **Author role:** Gate A coordinator.

**Publication rule (approved 2026-11-03, reaffirmed):** GitHub publication is handled
separately through the user's existing GitHub connection. Gate A never requested
credentials or tokens; all remote reads were anonymous (public clone, unauthenticated
`api.github.com` GETs). Publication and CI verification are NOT Gate A work.
Every patch stack referenced here carries its exact base SHA.

## 1. Published state — exact, verified

- **Published main HEAD:** `50b78304b627236937887737d9ed4b5eebebf1b8`
- **HEAD commit:** `Merge Phase 10b–16 engineering and research prototypes (#11)`,
  2026-11-03 18:54:53 +0300.
- **Merge parents:** `a1bb50be3c797025d96e5c744e615c632b0a8f83` (PR #10 main line) and
  `8a436a0b0c78c845f13b80d30f427272b296a98e` (integration branch tip).
- **Scratch clone used for all HEAD checks:** `~/workspace/zmbor-stabilization/main`
  (fresh anonymous clone made by Gate A; the older `~/workspace/zmbor-research/main`
  snapshot was NOT reused).
- **Historical phase baseline (for the old cumulative patches only):**
  `350ca1f3b2cd2aede0d5ec8491b36d2c8a552141` (2026-10-28). The working tree is still
  based on this commit with all phases uncommitted on top of it.

### Verdicts on the three starting claims

| # | Claim | Verdict |
|---|-------|---------|
| (a) | Phase 10b–16 integrated through PR #11, merge `50b7830…` | **CONFIRMED EXACTLY.** HEAD *is* that merge commit. Phase markers individually grepped in HEAD: 10b (guest reverb regs, `iop_import_tests.cpp`/`iop_spu2.h`), 12 (`discoverFromEntryPoint`), 13 (TODO_NAMED loud-every-call + numeric-syscall throw), 14 (`ResolveBlockConstant`), 15 (`Ps2TlbMissError` + `handleCacheOp`), 16 (`WantsRawGif` + ADR 0001 status "partially implemented (Stage 0)"). Phases 4 (SIF0) and 5 (test infra) also grepped in HEAD. Phases 6–11 are present by inheritance via parent `a1bb50b` (research-verified 2026-11-03 as "main through Phase 11"); Gate A did not re-grep each of 6–11 individually. Phase 17 made no tree changes by design (harness lives outside the tree), so its absence from the tree is correct, not a gap. |
| (b) | Phase 19 functions-only Ghidra integration passed local tests (650/650) but was not yet integrated in the last confirmed GitHub state | **CONFIRMED, with one precision.** The Phase 19 *integration wiring* (in-tree `ps2xRecomp/tools/ghidra/ExportPS2FunctionsHeadless.java` + README + `elf_parser.cpp` misaligned-start fix + 3 tests) is NOT in HEAD — proven by the HEAD↔working-tree diff (see §2). Nuance: HEAD *does* contain `research/poc1/ExportPS2FunctionsHeadless.java` — the POC 1 research artifact integrated by PR #11. That is not the Phase 19 integration; the two files must not be conflated. The 650/650 figure is the Phase 19 coordinator's reported local result, not re-run by Gate A (Gate A runs no suite builds by design; the working-tree binary state was not assumed). |
| (c) | Latest confirmed integration CI failed on Windows MSVC while Linux passed, with `ps2_memory.h(24,64)` C2039/C3861/C2512 | **CONFIRMED at job level; verbatim log lines UNVERIFIED (blocker recorded).** Latest main run `41668778457` (HEAD `50b78304`, 2026-11-03T15:54:57Z): `windows-msvc-x86_64` = failure at the Build step; `linux-gcc`, `linux-clang`, `linux-clang-asan-ubsan`, `linux-clang-fuzz-smoke`, `status-dashboard` = success. Run URL: https://github.com/m7kor/Zmbor/actions/runs/41668778457 . The quoted diagnostics could NOT be re-fetched: the job-logs API returns **403 "Must have admin rights to Repository"** for anonymous access. The underlying defect is nevertheless confirmed at code level (§5): line 24 uses `std::to_string` while the header includes no `<string>`. Gate B must re-confirm the verbatim lines with authorized access via the user's GitHub connection. |

## 2. Lineage — published HEAD → unpublished delta (the anti-duplication record)

### 2.1 Method (read-only)

Full recursive diff (`diff -r`, excluding `.git`, `mc0`, `mc1`, `__pycache__`, `*.pyc`)
between the scratch clone (HEAD) and the working tree (`~/workspace/zmbor-work/Zmbor`).
Result: **7 differing files + 11 only-in entries.** That small delta is itself the proof
that HEAD already contains Phases 4–16: everything else cancels out.

### 2.2 The unpublished delta vs HEAD, file by file

**Modified (6 files, all attributable):**

| File | Delta vs HEAD | Attribution |
|---|---|---|
| `CMakeLists.txt` (root) | +25 lines: sccache launcher block after `project()`, before any `add_subdirectory()` | Phase 20 |
| `ps2xRuntime/CMakeLists.txt` | −17 lines sccache block removed; +PGS option + ~55-line adapter block | Phase 20 (removal) + Phase 18 (addition) |
| `ps2xRecomp/src/lib/elf_parser.cpp` | **Exactly +20 lines, additions only**: `skippedMisaligned` + warning | Phase 19 |
| `ps2xTest/src/ps2_recompiler_tests.cpp` | +274 lines (3 Ghidra tests + helper fix) | Phase 19 |
| `ps2xTest/CMakeLists.txt` | +1 line: `src/gs_parallel_backend_tests.cpp` | Phase 18 |
| `ps2xTest/src/main.cpp` | +2 lines: declare + register `register_ps2_parallel_gs_tests()` | Phase 18 |

**New in working tree only (6 files):**

| File | Attribution |
|---|---|
| `ps2xRuntime/include/runtime/gs/gs_parallel_backend.h`, `ps2xRuntime/src/lib/gs/gs_parallel_backend.cpp`, `ps2xTest/src/gs_parallel_backend_tests.cpp` | Phase 18 (adapter, default-OFF, E2 FAILED — experimental, never default-on) |
| `ps2xRecomp/tools/ghidra/ExportPS2FunctionsHeadless.java`, `ps2xRecomp/tools/ghidra/README-GHIDRA-ORACLE.md` | Phase 19 |
| `docs/engineering/sccache.md` | Phase 20 |

**Present in HEAD only (must be preserved — a working-tree-derived stack would delete them):**

- `docs/architecture/NEXT_GENERATION_RECOMPILER_DESIGN.md`
- `docs/research/` (landscape, feasibility matrix, benchmarks)
- `research/poc1/`, `research/poc2/`, `research/poc3/` (incl. `research/poc1/ExportPS2FunctionsHeadless.java`, the POC 1 artifact — not the Phase 19 integration)

**README regression risk (confirmed by diff):** HEAD's `README.md` is NEWER (PR #11
badges/tagline/docs update); the working tree's `README.md` predates it. Any stack
derived from the working tree would regress the README. The forward stack must carry
HEAD's README unchanged.

### 2.3 Working-tree inventory (read-only `git status`/`git diff`)

- `git status --porcelain`: **65 lines** = 62 files with staged changes vs baseline
  `350ca1f` + 3 untracked files (`docs/engineering/sccache.md`,
  `ps2xRecomp/tools/ghidra/ExportPS2FunctionsHeadless.java`,
  `ps2xRecomp/tools/ghidra/README-GHIDRA-ORACLE.md`).
- 4 files carry BOTH staged and unstaged changes (`MM`): root `CMakeLists.txt`,
  `ps2xRuntime/CMakeLists.txt`, `ps2xRecomp/src/lib/elf_parser.cpp`,
  `ps2xTest/src/ps2_recompiler_tests.cpp`. Decomposition of the unstaged layer:
  root `CMakeLists.txt` +25 and `ps2xRuntime/CMakeLists.txt` −17 = Phase 20 hook move;
  `elf_parser.cpp` +20 and `ps2_recompiler_tests.cpp` +274 = Phase 19.
- The staged layer (62 files) is the Phase 18 cumulative state. The tree is therefore
  NOT a clean checkout of any single patch stack: it is Phase 18 (staged) + Phase 19
  (unstaged + untracked) + Phase 20 (unstaged + untracked). Gate G must not treat the
  index as a publication artifact.

### 2.4 THE LINEAGE RULE (binding on later gates)

- **The existing `350ca1f`-based cumulative patches in `~/workspace/your_files/` are
  historical records, NOT the publication stack.** They contain Phases 4–20, most of
  which (4–16) are already in published main. Replaying any of them onto current main
  would duplicate integrated work and, in the case of the working-tree state, delete
  HEAD-only files (§2.2) and regress the README.
- **The publication stack is built forward from published main HEAD
  (`50b78304b627236937887737d9ed4b5eebebf1b8`),** as new, reviewable patches:
  Phase 18 adapter (experimental, default-OFF) → Phase 19 Ghidra integration →
  Phase 20 sccache → stabilization changes. Each patch's base SHA is recorded at
  creation; if HEAD moves before publication, the stack is rebased and re-validated,
  never force-applied.
- **Forward-stack validation procedure (defined by Gate A; executed by later gates):**
  1. Record the base SHA (current main HEAD) at stack creation; re-verify it has not
     moved before any publication step.
  2. Apply the stack in order to a fresh anonymous clone of main at the recorded SHA
     (`git apply --check`, then apply). Every patch must apply cleanly onto its
     predecessor; no patch may contain a file whose HEAD content already equals the
     patch's post-image (that is a duplicated integration — reject the patch).
  3. Recursive tree comparison of the stacked clone against the expected end-state
     tree (excluding `.git`, `mc0`/`mc1`, `__pycache__`) must show **zero differences**.
  4. Negative checks: HEAD-only files (`docs/research/`, `docs/architecture/NEXT_GENERATION_RECOMPILER_DESIGN.md`,
     `research/poc1|2|3/`, updated `README.md`) must still be present and byte-identical
     after stacking.
  5. Test gates on the stacked tree (Gate G executes): `ps2x_tests` Release and
     ASan+UBSan, full CTest under `xvfb-run -a`, IOP standalone 4/4.
  6. Publication itself (review branch, PR, CI) happens only via the user's GitHub
     connection when authorized — never a direct push to main, never a force-push.

## 3. Branches, PRs, workflows, CI

### 3.1 Branches (public API, cached `api/branches.json`, 56/60 rate-limit remaining)

- `main` @ `50b78304` (HEAD, confirmed).
- `integration/phase10b-16-research-2026-11-03` @ `8a436a0b` (the merged PR #11 branch;
  now redundant — HEAD contains its merge).
- `automation/meta-muse-zmbor-setup-20261102` @ `350ca1f3` (the old phase baseline).
- Docs branches: `docs/readme-after-pr7`, `docs/readme-phase11-system-overview-2026-11-03`,
  `docs/status-dashboard`; `maint/handoff-and-ci`; `ultimate/*` series (phase4/5/6/7
  hardware-accuracy, architectural-correctness, native-compatibility, full-system,
  ps2-hardware-system-engineering). None of these were examined beyond listing;
  no stabilization work may be based on them without a fresh lineage check.
- **Open PRs: none.**

### 3.2 Workflow (single file, published HEAD `.github/workflows/build.yml`, 219 lines)

Jobs: `linux-gcc-clang` (gcc+clang matrix, Ninja, Release, `-msse4.1`), `status-dashboard`
(inventory/contract validators, must stay green), `linux-clang-sanitizers` (ASan+UBSan,
`halt_on_error`, Z3 integer proofs, e2e), `linux-clang-fuzz-smoke` (6 targets × 60 s),
`windows-msvc` (x86_64 only, `setup-msvc-dev`, default VS generator, Release, unit tests
+ fpenv self-tests + VU1 bench; **no e2e step by documented design** — hosted Windows
runners have no OpenGL/WGL driver and the runtime opens a window even when hidden;
comment states e2e runs on Linux jobs and in the local Windows gate).

- **`continue-on-error`: 0 occurrences** in the workflow. No required check is hidden
  behind it. (`fail-fast: false` on the matrices is legitimate — it lets all matrix
  legs report, it does not hide failures.)
- **e2e selection (CONFIRMED GAP, see register S8):** Linux and sanitizer jobs run
  `ctest -R "^e2e_(basic|branchdelay|vu_macro)$"` — only the original three cases.
  `e2e_stripped` (Phase 12) and `e2e_gs_sprite` are NOT in CI.

### 3.3 CI runs (public API, cached `api/runs.json`, `api/run_main_jobs.json`)

- Latest main run `41668778457` (HEAD `50b78304`): **conclusion `failure`, caused solely
  by `windows-msvc-x86_64` failing at its Build step** (job `126031188373`).
  All other jobs succeeded. URL: https://github.com/m7kor/Zmbor/actions/runs/41668778457
- The preceding ~12 runs on the integration branch also concluded `failure` (same
  Windows failure pattern is the plausible cause, but Gate A did not fetch each job
  list; do not assert it for those runs).
- **Blocker (process, not product):** anonymous job-log download returns
  `403 "Must have admin rights to Repository"`. Verbatim CI log lines are therefore
  UNVERIFIED by Gate A; Gate B/G must confirm them with authorized access.

## 4. Toolchain / dependency state (read-only, published HEAD)

- **C++ standard: C++20**, set per-module (`ps2xAnalyzer`, `ps2xRecomp`, `ps2xRuntime`,
  `ps2xStudio`, `ps2xTest` CMakeLists each set `CMAKE_CXX_STANDARD 20` +
  `REQUIRED ON`). **No root-level standard is set.** `ps2xIOP/CMakeLists.txt` sets no
  standard in its own file (see register S12).
- **Local VM toolchain (for context, not a CI claim):** CMake 3.28.3, g++ 13.3.0,
  Ninja NOT installed, **no `clang++` on this VM** — local Clang checks were impossible
  (see §5.4). CI covers Clang; Gate B must still run the header check under Clang
  somewhere (CI or another host).
- **FetchContent pins in HEAD:** sse2neon `v1.9.1` (root, for non-MSVC SIMD compat);
  ELFIO `Release_3.12`, toml11 `v4.4.0`, fmt `12.1.0`, libdwarf-code `v2.2.0`,
  rabbitizer `1.14.3` (ps2xRecomp); nlohmann_json `v3.11.3` (ps2xAnalyzer, `GIT_SHALLOW
  TRUE`); raylib `5.5`, imgui `v1.92.7-docking`, rlImGui `Raylib_5_5` (ps2xRuntime,
  GUI-gated); parallel-gs `cc6184af…` (working tree only, Phase 18, default-OFF).
  No pin drift was found between HEAD declarations and the local dependency cache
  (`~/workspace/zmbor-deps/`).
- **Sccache state (Phase 20, completed):** binary `~/workspace/zmbor-tools/sccache-v0.19.0/sccache`,
  local-disk cache only, hook now root-scoped (see §2.2). The sccache coordinator's
  build dir (`~/workspace/zmbor-build-sccache`) was not touched by Gate A.

## 5. Windows root-cause diagnosis (read-only; fix belongs to Gate B)

### 5.1 The defect, exactly

`ps2xRuntime/include/runtime/ps2_memory.h` (published HEAD, lines 4–24):

```cpp
#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
...
struct Ps2TlbMissError : std::runtime_error
{
    uint32_t vaddr;
    explicit Ps2TlbMissError(uint32_t v)
        : std::runtime_error("TLB miss for address: 0x" + std::to_string(v)), vaddr(v)
    {
    }
};
```

- **Missing declaration:** `<string>` is never included, yet line 24 uses `std::to_string`
  and `std::string` concatenation. That is the whole root cause.
- **Why GCC/Clang accept it:** libstdc++'s `<iostream>` transitively pulls in `<string>`
  (and hence `std::to_string`) — proven indirectly by the standalone audit: `ps2_memory.h`
  compiles cleanly under GCC. MSVC's header graph does not provide `std::to_string`
  through this include set, so the same TU fails there. This is the classic
  transitive-include defect class, not a compiler bug and not a code-generation issue.
- **C2512 is a secondary (cascade) diagnostic, not an independent defect:** `std::runtime_error`
  has no default constructor in the standard. When the base-class initializer expression
  is ill-formed (`std::to_string` unresolved → C2039 + C3861), MSVC falls back to
  attempting default construction of the base and reports C2512. Fixing the missing
  `<string>` removes all three diagnostics together; Gate B must confirm that by the
  MSVC run (if C2512 survived a `<string>` fix, it would be reclassified as independent).
- **Separate formatting defect (same line, its own register item S7):**
  `"TLB miss for address: 0x" + std::to_string(v)` formats `v` in **decimal** after a
  `0x` prefix (e.g. `v = 0x1F900000` renders as `0x532676224`). The address value and
  the exception behavior must be preserved; only the rendering changes. Minimal remedy:
  hex formatting through standard means (`std::ostringstream` with `std::hex`, or a
  small `snprintf` into a `std::string`) — explicitly NOT a custom formatting framework.

### 5.2 Header self-containment audit (GCC standalone TUs, published HEAD)

Method: for each of the 70 public headers (`ps2xRuntime/include` 36,
`ps2xRecomp/include` 22, `ps2xAnalyzer/include` 7, `ps2xIOP/include` 5), compiled
`#include "<header>"` + empty `main` with `g++ -std=c++20 -fsyntax-only` against the
module include dirs plus the local dependency cache. Result: **67 pass, 3 fail.**

| Header | Failure (GCC-proven) | Class |
|---|---|---|
| `ps2xRuntime/include/runtime/ps2_memory.h` | Passes under GCC (transitive `<string>`); **fails under MSVC** per the CI job failure + code reading (§5.1) | Missing `<string>` (P0, S1) |
| `ps2xRuntime/include/types.h` | Uses `size_t`/`usz` with only `<cstdint>` included (`using usz = size_t;` at line 5) | Missing `<cstddef>` (P1, S2) |
| `ps2xRecomp/include/ps2recomp/control_flow_utils.h` | Uses `ps2recomp::Instruction` (defined in `ps2recomp/types.h:14`) but includes only `ps2recomp/instructions.h` (macros only, no type definition); every full-build consumer includes `types.h` first, masking it | Missing include / include-order dependence (P1, S2) |
| `ps2xRuntime/include/ps2_stubs.h` | Includes `"Stubs/Unimplemented.h"`, which lives at `ps2xRuntime/src/lib/Kernel/Stubs/Unimplemented.h` — a **private src path**, not a public include dir; fails standalone under public include paths | Public header reaching into a private directory; path-contract defect (P1, S2). Generated TUs also emit `#include "ps2_stubs.h"` (see S14). |

### 5.3 What Gate B must still establish (Gate A could not)

- MSVC standalone results for all 70 headers (no MSVC on this VM).
- Clang standalone results (no `clang++` on this VM; `clang-cl` likewise unavailable).
- Whether the verbatim C2039/C3861/C2512 lines reappear on the current HEAD with
  authorized log access, and that they vanish with the `<string>` fix alone.
- The full public-header list for the working tree's NEW headers (`gs_parallel_backend.h`
  — Phase 18, not in HEAD, not audited by Gate A; Gate B audits it against the forward
  stack, not against HEAD).

## 6. Read-only audits for later gates

### 6.1 SLTIU constant resolution — DEFECTIVE (confirmed by code reading)

`ps2xRecomp/src/lib/elf_parser.cpp`, `ResolveBlockConstant` (working tree; identical
in HEAD — Phase 14 is integrated). The scan tracks a constant per GPR; immediates:

```cpp
const uint32_t imm16 = IMMEDIATE(raw);                          // zero-extended 16-bit field
const int32_t simm = static_cast<int32_t>(static_cast<int16_t>(imm16));  // sign-extended
...
if (opcode == OPCODE_SLTI)   // correct: signed compare against simm
    setReg(rt, ... (static_cast<int32_t>(*known[rs]) < simm) ? 1u : 0u ...);
if (opcode == OPCODE_SLTIU)  // DEFECTIVE: unsigned compare against zero-extended imm16
    setReg(rt, ... ((*known[rs] < imm16) ? 1u : 0u) ...);
```

**MIPS/R5900 semantics:** SLTIU sign-extends the 16-bit immediate to 32 bits FIRST,
then compares both operands as UNSIGNED. The code compares against the zero-extended
field instead, which differs exactly when the immediate is negative (`imm16 >= 0x8000`).

**Counterexample (proves a false PROVEN value):** `sltiu $t0, $s0, -1` with
`$s0 = 0x00010000` (65536). Correct: sign-extended imm = `0xFFFFFFFF`;
`65536 < 0xFFFFFFFF` → 1. Code: `65536 < 0xFFFF` (65535) → 0. The analyzer's
"proven" tier therefore claims a value the block does not prove — violating the
function's own stated soundness direction ("may only fail to resolve, never claim a
value the block does not prove"). A false 0/1 can feed ADDU/OR chains toward a JR/JALR
register (direct 0/1 targets are rejected by the code-section/alignment guards, so
exploitation requires a chain — hence P1, not P0; see register S3).

**Neighbouring code examined and NOT defective:** SLTI (signed, correct); ADDIU/ADDI
(int64 + `simm`, overflow-guarded, correct); ORI/ANDI/XORI (zero-extension is correct
for those opcodes); LUI (correct). The jump-table bound sites (same file, SLTIU/SLTI
bound read as `boundInst.immediate`) are **fail-safe, not wrong**: a negative SLTIU
bound yields `numCases` 32768–65535, which exceeds the 1000 cap and rejects the table
(lost precision, never a false table). Gate C fixes only `ResolveBlockConstant`'s
SLTIU arm and adds boundary tests; it must not "fix" the bound sites into accepting
huge bounds.

### 6.2 Scheduler transfer / C++ object lifetime — FRAMING CORRECTION (confirmed)

**There is no C `setjmp`/`longjmp` in product code.** Grep over `ps2xRuntime/`,
`ps2xRecomp/`, `ps2xAnalyzer/`, `ps2xIOP/` finds `setjmp`/`longjmp` only in
(a) the generated symbol-database string data, (b) `ps2xAnalyzer`'s function-classifier
name list, and (c) IOP guest-import stubs for guest `setjmp`/`longjmp` (unrelated guest
ABI surface). The Phase 15 mechanism is a plain C++ exception:

```cpp
// ps2xRuntime/include/runtime/ee_scheduler.h:19-22
// This exception is the EE equivalent of a longjmp to the dispatcher.  It is
// not an error and must only be caught at EeScheduler::run().
struct EeDispatcherTransfer final { };
```

C++ exceptions run destructors during unwinding, so the "longjmp skips destructors"
UB class **does not apply** to this mechanism as implemented. The brief's longjmp
concern is reclassified, not confirmed, as follows (all inside `EeScheduler::run()`,
which starts at `EeScheduler.cpp:159` — consistent with the "only caught at run()"
contract):

- Throw sites (e.g. `PS2Runtime::handleBreak`, `ps2_runtime.cpp:1612-1620`): raise the
  COP0 exception state FIRST (`raiseCop0Exception`), then `throw EeDispatcherTransfer{}`
  so the generated block aborts instead of its terminal branch overwriting the vector PC.
- Catch sites: `EeScheduler.cpp:222` (resume-completion callback, swallowed, loop
  continues), `:262` (invocation `onComplete` callback, swallowed, loop continues),
  `:310-322` (around the guest function call `function(m_rdram, &context, &m_runtime)`,
  resets `m_guestExecuting`/`m_insideInterrupt`, loop continues to redispatch at the
  vector PC). A trailing `catch (...)` stops the scheduler (`m_running = false`).
- **Residual risks for Gate C (hypotheses, NOT confirmed defects):**
  (R1) A `noexcept` boundary anywhere on the throw→catch chain (generated entry point
  → runtime helper → scheduler) would call `std::terminate`. Gate A grepped the two
  emitter files for `noexcept` and found none, but a full call-chain audit is Gate C work.
  (R2) The two swallow sites (222, 262): a transfer thrown from a nested guest call
  inside a callback is swallowed and the loop continues — Gate C must trace whether
  exception PC/state survives that path or whether a nested abort can silently resume
  the wrong thread/pc. Adversarial tests: exception through a generated block with live
  locals; exception from inside a completion callback; exception from an interrupt
  invocation path.

### 6.3 GS CLUT restoration — PROVABLY INSUFFICIENT as specified (confirmed by code)

Stage 0 swap contract (`GS::setRasterBackend`, `gs_frontend.cpp:1669-1705`): on swap,
the frontend re-issues **only the last** `LoadClut` call's arguments
(`m_lastLoadClutTex0`/`m_lastLoadClutTexclut`, cached in `issueLoadClutUnlocked`,
`gs_frontend.cpp:1713-1721`, on EVERY TEX0/TEX2 register write at lines 1268/1299 —
including writes whose CLD makes them backend no-ops). The CPU backend
(`gs_cpu_backend.cpp:567-601`) implements CLD faithfully: CLD 0/6/7 → no-op;
CLD 1 → load; CLD 2/3 → record CBP slot + load; CLD 4/5 → load only if the CBP
differs from the recorded slot (`m_clutCbp[0..1]`, zero-initialized per backend);
`LoadClutUnlocked` snapshots palette entries from VRAM into `m_clut[]`
(CSM1 swizzle + CSM1-suffix partial loads; CSM2 via `texclut` cbw/cou/cov).
`TextureFlush` invalidates the texture page cache only — it does NOT touch `m_clut`.

**Consequences (each is a concrete failure mode, not a vague worry):**

| Sequence | Why restoration fails |
|---|---|
| `CLD=1` load palette → `CLD=0` TEX0 write (texture switch without reload) → swap | Last cached call is the CLD=0 no-op; the new backend's `m_clut` stays all zeros. Palette lost. |
| Last load `CLD=4` (or 5) with `CBP=0` → swap | Fresh backend has `m_clutCbp = {0,0}`; the conditional sees CBP already recorded and skips the load. Palette lost. |
| Multiple partial loads (different CBP/CSA slices, incl. CSM1-suffix partial loads) → swap | Only the last slice is re-issued; earlier slices are zeros in the new backend. |
| Palette change in VRAM + `TEXFLUSH` → swap | TEXFLUSH does not restore CLUT; the re-issue reads current VRAM, so this one actually works — recorded to prevent a false fix. |

**What IS sufficient:** a single effective load covering the whole active palette as
the last TEX0/TEX2 write. **What is NOT sufficient, and must not be claimed:**
transfer-idle or startup-only swap timing does not fix any row above — the gap is
palette *history*, not transfer state. Gate E options (in order of contract cleanliness):
(1) startup-only backend selection with NO mid-stream swap at all (the Stage 2 adapter
already selects at startup only — extend that to the contract); or (2) ordered replay
of the full effective LoadClut history against the fresh backend (replay reproduces
`m_clut` + `m_clutCbp` deterministically from current VRAM; no-op calls replay harmlessly).
Do NOT add cross-backend snapshot copying (the brief forbids fragile state copying
between incompatible implementations). CSM1 and CSM2 are both covered by replay since
the same `tex0`/`texclut` args are re-delivered; CSM2 was not fixture-covered in Stage 1
(CSM1 only) — a Gate E fixture gap.

### 6.4 Ghidra loader validation — what it checks and what it does not (confirmed)

`ElfParser::loadGhidraFunctionMap` (working tree, Phase 19 state). **Checked:**
file opens (else warning + false); each row needs 4 comma fields (else silently skipped);
`start` must lie in an executable section (`FindFunctionSectionByAddress`, else
`skippedNonExecutable`); **misaligned starts skipped** (Phase 19, `start & 0x3`);
`end` clamped to the section (`ClampFunctionEndToSection`), `end <= start` skipped
(`skippedInvalidRange`); same-start duplicates collapsed by sort+unique keeping the
named entry over an auto-generated one; auto-generated extras (`sub_`/`FUN_`/`LAB_`/`DAT_`
prefixes per `IsAutoGeneratedName`) not in the map's start set are erased (the map-set
rule, which also applies to discovered functions); `Size` field is read but ignored
by design (Phase 19: Size is body address count, not byte size); uncontested CSV ends
survive the merge while sized symbols override downstream (precedence lives in
`extractFunctions`/`addOrMerge`, not in the loader).

**NOT checked (each is a register gap, Gate D):**
1. End alignment (an odd/misaligned `end` is clamped, never rejected).
2. Overlapping ranges between distinct map rows (same-start collapse only; partial
   overlaps both promote — downstream merge does not adjudicate them).
3. Whether the row's bytes decode as code (executable-section membership only; data
   sitting in `.text` passes).
4. Delay-slot starts (Phase 19 documented gap: a start landing on a delay slot is not
   validated).
5. Malformed-row accounting (rows failing field parse or `stoul` are skipped silently
   via `catch (...)` — no warning, no count; a truncated file is indistinguishable
   from a clean one).
6. Header row assumed, never validated (a headerless file silently loses its first row).
7. `std::stoul(..., 0)` base-0 parsing: a leading-`0` field parses as OCTAL
   (`"010"` → 8). The exporter writes hex, so this is latent, not live — but an
   adversarial or hand-edited CSV can shift addresses by misparsing.
8. No row-count/file-size bound (local file only, so a minor robustness note, not a
   security claim).
9. **The `entry_*` hazard (demonstrated, Phase 19):** `entry_*` is NOT auto-generated
   per `IsAutoGeneratedName`, so label rows (e.g. switch-block labels, intra-function
   branch targets — measured 0/19 precision in Phase 19) promote as *named* extras
   through this same path. The loader cannot filter them because they share the CSV
   format. Phase 19's verdict (functions-only export; label port REJECTED) must be
   preserved: Gate D must not reintroduce label promotion under another name, and
   must add adversarial fixtures for label-shaped rows.

### 6.5 Phase 15 exception surface — audit scope note (honest boundary)

Gate A confirmed the abort *pattern* (raise state → throw transfer → catch in `run()` →
redispatch) and the `handleBreak`/`handleCop0` sites' structure. It did NOT complete the
full §4.1 matrix (alignment exceptions per width/direction, vector selection,
BadVAddr/EPC/Cause/BD/EXL/ERET per fault, privilege/region rules, CACHE validation
per encoding, BREAK/TRAP termination, delay-slot exception PC, terminal-branch
overwrite protection per fault). That matrix is Gate C work; it is listed as an open
audit item, not as verified and not as failed.

## 7. Defect register

**Evidence levels used:** `confirmed-by-code-reading` (source inspected, quoted),
`confirmed-by-log` (CI API/log output), `hypothesis-needing-reproduction` (plausible,
not yet demonstrated), `unverified` (could not be checked in Gate A; blocker stated).
**Verification outcome for every item below:** not yet fixed — verification pending
(Gate A fixes nothing). Severity: P0 = correctness/release blocker; P1 = important
reliability issue; P2 = improvement or process item.

### Systemic priorities (top of register, per addendum)

**S1 — P0 — Windows MSVC build failure: `ps2_memory.h` missing `<string>`.**
Locations: `ps2xRuntime/include/runtime/ps2_memory.h:4-24` (include list lacks `<string>`;
line 24 uses `std::to_string`). Root-cause hypothesis (confirmed, not a guess):
transitive-include dependence — GCC's `<iostream>` supplies `<string>`, MSVC's header
graph does not through this include set. Evidence: `confirmed-by-code-reading` +
`confirmed-by-log` at job level (run `41668778457`, job `windows-msvc-x86_64`, Build
step failure; verbatim C2039/C3861/C2512 lines `unverified` — 403 blocker, §3.3).
Adversarial repro plan (Gate B): standalone TU `#include "runtime/ps2_memory.h"` +
empty `main` under MSVC (must fail before, pass after adding `<string>`); repeat
under GCC/Clang to prove no regression; then full Windows CI run. Fix strategy:
add `#include <string>` to `ps2_memory.h` (and audit-order fix per §5.2, not a
scattershot include dump). Gate: B.

**S2 — P1 — Header self-containment: three GCC-proven failures + one MSVC-only failure.**
Locations: `ps2xRuntime/include/types.h` (missing `<cstddef>` for `size_t`),
`ps2xRecomp/include/ps2recomp/control_flow_utils.h` (missing `ps2recomp/types.h` for
`Instruction`), `ps2xRuntime/include/ps2_stubs.h` (includes private
`Stubs/Unimplemented.h` outside public include dirs), plus S1's `ps2_memory.h` under
MSVC. Root cause: headers relying on transitive includes or consumer include order.
Evidence: `confirmed-by-code-reading` + GCC standalone audit 67/70 pass (§5.2); MSVC
impact of the three GCC failures is `hypothesis-needing-reproduction` (same defect
class as S1, but not yet observed under MSVC). Adversarial repro plan (Gate B):
standalone TU per public header under MSVC, GCC, Clang (and `clang-cl` where
practical), all 70 + the forward stack's new `gs_parallel_backend.h`; any failure is
a register item, not a reason to add the header to an exclusion list. Fix strategy:
add the missing include at the owning header; for `ps2_stubs.h`, either move the
private header's declaration into the public contract or document the required
additional include dir as part of the header's contract (Gate B decides on evidence,
not by guessing consumer paths). Gate: B.

**S3 — P1 — SLTIU sign-extension defect in block-local constant resolution.**
Locations: `ps2xRecomp/src/lib/elf_parser.cpp`, `ResolveBlockConstant`, SLTIU arm
(working tree and HEAD identical). Root cause: unsigned compare against the
zero-extended 16-bit field instead of the sign-extended immediate (MIPS SLTIU
sign-extends first, then compares unsigned). Evidence: `confirmed-by-code-reading`
with counterexample (§6.1). Adversarial repro plan (Gate C): unit tests —
`sltiu` with negative immediates (`-1`, `-32768`, `0x8000`) against `rs` values
straddling `0xFFFF`/`0x10000`/`0xFFFFFFFF`; `sltiu` result feeding ADDU/OR chains
into a JR/JALR register (wrong-target proof); positive-immediate controls (must be
unchanged); mutant check (revert the arm → exactly the new tests fail). Fix strategy:
compare `*known[rs]` against `static_cast<uint32_t>(simm)` (the already-computed
sign-extended value). Gate: C.

**S4 — P1 — GS CLUT restoration: last-load-only re-issue is provably insufficient.**
Locations: `ps2xRuntime/src/lib/gs/gs_frontend.cpp:1268,1299,1669-1721`,
`ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp:567-601` (+ `m_clut`/`m_clutCbp` state).
Root cause: the swap contract caches one call's arguments while palette state is
cumulative across calls and conditional on per-backend CBP slots that reset to zero.
Evidence: `confirmed-by-code-reading` with three concrete failure sequences (§6.3).
Adversarial repro plan (Gate E): swap-after-sequences — (a) CLD=1 load then CLD=0
write then swap, render indexed sprite, compare frame; (b) CLD=4/CBP=0 last load
then swap; (c) two CBP/CSA slices then swap; (d) CSM1-suffix partial loads then swap;
(e) CSM2 multi-slice (new fixture — Stage 1 covered CSM1 only). Fix strategy:
startup-only selection (no mid-stream swap) OR ordered full-history replay; never
cross-backend snapshot copying. Gate: E.

**S5 — P1 (verification item, NOT a confirmed defect) — Scheduler transfer / C++ lifetime:
framing correction + two residual risks.**
Locations: `ps2xRuntime/include/runtime/ee_scheduler.h:19-22`,
`ps2xRuntime/src/lib/Kernel/EeScheduler.cpp:159-330`,
`ps2xRuntime/src/lib/ps2_runtime.cpp:1612-1620` (representative throw site).
Root-cause hypothesis: NONE for longjmp UB — **no C `setjmp`/`longjmp` exists in
product code** (verified by grep); the mechanism is a C++ exception, which runs
destructors during unwinding. Evidence: `confirmed-by-code-reading` (absence + struct
def + all three catch sites inside `run()`). Residual risks (both
`hypothesis-needing-reproduction`): (R1) a `noexcept` boundary on the throw→catch
chain would `std::terminate` (emitter files grepped: none found; full chain unaudited);
(R2) swallow sites at `:222`/`:262` may lose exception PC/state for nested aborts.
Adversarial repro plan (Gate C): exception through a generated block with live
non-trivial locals (destructor-observing test); exception from inside a completion
callback; exception from an interrupt invocation; audit every function on the
throw→catch chain for `noexcept`. Fix strategy: only if R1/R2 reproduce — otherwise
close with the tracing evidence and a regression test pinning the contract. Gate: C.

**S6 — P1 — ELF/CFG candidate validation: loader accepts speculative rows as named
functions; proven/guarded/speculative separation is enforced downstream, not at the
boundary.**
Locations: `ps2xRecomp/src/lib/elf_parser.cpp`, `loadGhidraFunctionMap` (gaps §6.4),
`IsAutoGeneratedName` (prefixes `sub_`/`FUN_`/`LAB_`/`DAT_` only), merge in
`extractFunctions`/`addOrMerge`. Root cause: the loader validates section membership,
alignment (Phase 19), and clamped range — but not decodability, delay-slot position,
end alignment, or overlap — and the `entry_*` label shape is indistinguishable from a
real function row in the CSV format. Evidence: `confirmed-by-code-reading` +
Phase 19's measured label precision 0/19 and its pinned promotion test (the hazard
is demonstrated, not hypothetical). Adversarial repro plan (Gate D): CSV fixtures —
misaligned start (covered), non-exec address (covered), inverted end, end inside
another function (overlap), start on a delay slot, start on data bytes inside
`.text`, `entry_*` label rows (switch blocks, branch targets), duplicate starts with
conflicting ends, headerless file, octal-looking fields, 100k-row file; each must
either promote correctly or be rejected with a warning count. Fix strategy: extend
loader validation per gap (never by weakening the merge precedence: symbols >
map > extras > discovery, and never by reintroducing label promotion). Gate: D.

### Further register items

**S7 — P2 — TLB-miss message renders decimal after a `0x` prefix.**
Location: `ps2_memory.h:24`. Evidence: `confirmed-by-code-reading`. Adversarial repro:
construct `Ps2TlbMissError(0x1F900000)` and assert the message contains `1f900000`
(hex), not `532676224`. Fix: standard hex formatting (`ostringstream`/`snprintf`);
preserve value and exception behavior; no custom formatting framework. Gate: B
(same file as S1 — same patch group, separate test).

**S8 — P1 — CI e2e selection omits `e2e_stripped` (and `e2e_gs_sprite`).**
Location: `.github/workflows/build.yml` (Linux + sanitizer jobs:
`-R "^e2e_(basic|branchdelay|vu_macro)$"`). Evidence: `confirmed-by-code-reading`.
The stripped-ELF path — the highest-value analyzer regression (Phase 12) — has no CI
coverage, so a discovery regression would ship silently. Fix strategy (Gate F): add
`e2e_stripped` (and `e2e_gs_sprite` if the runner supports it) to the Linux/sanitizer
e2e selections; the Windows job's lack of e2e is documented by design (no WGL driver)
and stays, with the local Windows gate covering it. Gate: F.

**S9 — P2 (process) — Anonymous CI log access is blocked (403).**
Evidence: `confirmed-by-log` (API response quoted §3.3). Impact: independent CI
verification from outside the repo's access list is limited to job/step conclusions.
Fix strategy: none in code — Gate B/G confirm verbatim diagnostics through the user's
GitHub connection. Gate: B/G.

**S10 — P2 (process) — Working tree / index is not a publication artifact.**
Evidence: `confirmed-by-code-reading` (§2.3). A stack generated from this tree's index
would bake in Phase 18 state as if it were new and miss the Phase 19/20 unstaged
layer. Fix strategy: the §2.4 forward-stack procedure (build from HEAD, never from
this index). Gate: G.

**S11 — P2 — README regression risk if the stack is derived from the working tree.**
Evidence: `confirmed-by-code-reading` (HEAD README newer, §2.2). Fix strategy:
exclude README from the working-tree delta; carry HEAD's version. Gate: G.

**S12 — P2 — IOP standalone build sets no C++ standard in its own CMakeLists.**
Location: `ps2xIOP/CMakeLists.txt` (no `CMAKE_CXX_STANDARD` line; other modules set
C++20 explicitly). Evidence: `confirmed-by-code-reading`; actual standard used is
`hypothesis-needing-reproduction` (may inherit from a parent scope or fall back to
the compiler default). Adversarial repro (Gate B/F): configure the IOP standalone
build with `-Werror` on standard-sensitive constructs or print the effective
`CMAKE_CXX_STANDARD` from the cache. Fix strategy: set C++20 explicitly if the
effective standard is not C++20. Gate: B/F.

**S13 — P2 (verification item) — Generated-code include path for `ps2_stubs.h`.**
Generated TUs emit `#include "ps2_stubs.h"`, and that header includes private
`Stubs/Unimplemented.h` (S2). The e2e programs pass, so some include path must cover
it — but Gate A did not verify which one. Evidence: `hypothesis-needing-reproduction`.
Adversarial repro (Gate C): generate a stub-using program with ONLY the public
include dirs on the command line and record whether it compiles; if it fails, the
generated-code build contract must name the Kernel include dir explicitly. Gate: C.

**S14 — P2 (open audit, not a defect) — Phase 15 §4.1 exception matrix incomplete in
Gate A.** Alignment per width/direction, vector selection, BadVAddr/EPC/Cause/BD/EXL/
ERET per fault, privilege/region rules, CACHE per encoding, BREAK/TRAP termination,
delay-slot exception PC, terminal-branch overwrite protection per fault — all
UNVERIFIED by Gate A (only the abort pattern was traced). Gate C owns the matrix;
no claim is made here in either direction.

### Explicitly examined, NOT defects (recorded so later gates do not re-litigate)

- Jump-table bound SLTIU sites: fail-safe rejection on huge bounds (§6.1).
- `TextureFlush` not touching `m_clut`: correct snapshot semantics, not a bug (§6.3).
- Phase 17 absence from the tree: by design (harness is external).
- `research/poc1/ExportPS2FunctionsHeadless.java` in HEAD: research artifact, not the
  Phase 19 integration (§1, claim b).
- Windows job's lack of an e2e step: documented by design (no WGL driver); the gap
  is the Linux-side selection (S8), not the Windows job.
- No `continue-on-error` in CI (§3.2). No open PRs (§3.1).

## 8. Coverage gaps and semantic risks (summary for Gates C–F)

- **Analyzer:** SLTIU false-proof (S3); loader gaps (S6); no CI for stripped e2e (S8);
  cross-block constant propagation still unproven by design (documented Phase 14
  limitation — future work, not a defect).
- **Runtime/exceptions:** R1/R2 (S5); full §4.1 matrix open (S14); KUSEG identity-mapping
  remains a documented model choice (Phase 15 — do not invent translation).
- **GS:** CLUT restoration (S4); CSM2 fixture gap; E2 failures (F-flat edge pixels,
  F-tex bilinear) remain open pending hardware-correct edge/interpolation evidence —
  the adapter stays default-OFF experimental; Stage 3 is NOT reconsidered in this
  program without that evidence.
- **Ghidra:** label promotion stays REJECTED (S6); delay-slot starts unvalidated;
  end-exactness beyond the synthetic corpus undemonstrated; Ghidra/extension version
  changes require re-measurement.
- **Historical baselines not re-established by Gate A:** ps2autotests 53/55 ladder
  (Gate F must re-establish reproducibility before using it as a gate; historical
  unmatched results must not become silent skips); the 650/650 Phase 19 figure and
  the 650/650 Phase 20 sccache figure are coordinator-reported local results, not
  Gate A re-runs.
- **Hardware evidence:** none claimed anywhere in this report. No component is marked
  VERIFIED on model evidence. `components.json` was not touched and must not be
  upgraded by stabilization work without hardware-backed evidence.

## 9. Readiness for Gates B–G

- **Gate B can start immediately.** Its entry criteria are met: the Windows defect is
  isolated to a single missing include (S1) with a known fix, the header audit gives
  it an exact 3+1 target list (S2), and the formatting item (S7) rides the same file.
  Blockers Gate B must route around: no MSVC/Clang on this VM (use the user's GitHub
  connection for CI verification + a Clang host if available); anonymous log access
  is 403 (confirm verbatim diagnostics via authorized access).
- **Gate C entry:** S3 (with counterexample + fix), S5 (R1/R2 tracing plans), S13
  (generated include path), S14 (full §4.1 matrix). No Gate C work is unblocked by
  guessing — every item has a file/line anchor.
- **Gate D entry:** S6 gap list + adversarial fixture list (§6.4). Hard constraint:
  label promotion stays rejected; functions-only export is the approved direction.
- **Gate E entry:** S4 failure sequences + CSM2 fixture gap. Hard constraints: no
  tolerance widening, no golden modification, adapter stays default-OFF, no Stage 3.
- **Gate F entry:** S8 (e2e CI selection), S12 (IOP standard), ps2autotests
  re-establishment, sccache hook already complete (Phase 20 — do not modify its files
  without coordinating; the hook is root-scoped and measured, not a Gate F task).
- **Gate G entry:** §2.4 forward-stack procedure + S10/S11 negative checks + S9
  authorized-access requirement. Nothing in Gate A blocks Gate G's definition work,
  but Gate G execution requires Gates B–F evidence.
- **Sequencing (reaffirmed):** Gate A completes with this report. Gates B–G follow
  sequentially. The completed Phase 20 sccache milestone unblocks later gates but does
  not change gate order. Ledger protection stands: Gate A was read-only on the ledger;
  later gates append-only with verified backups (symlink verified first, every time).

## 10. Sources and evidence pointers (all read-only)

- Scratch clone: `~/workspace/zmbor-stabilization/main` @ `50b78304b627236937887737d9ed4b5eebebf1b8`.
- API caches: `~/workspace/zmbor-stabilization/api/{branches,pulls_open,runs,run_main_jobs}.json`
  (56/60 rate-limit remaining after Gate A calls).
- Tree diff: `/tmp/gatea_treediff.txt` (ephemeral; the §2.2 tables are the durable record).
- Header audit: `/tmp/hdr_audit2.sh` (ephemeral; §5.2 is the durable record).
- Run URL: https://github.com/m7kor/Zmbor/actions/runs/41668778457
  (job `windows-msvc-x86_64` failure; log download 403 — see §3.3).
- Working tree (read-only reference): `~/workspace/zmbor-work/Zmbor` @ base `350ca1f`
  + uncommitted Phases 18/19/20 (inventory §2.3 — NOT a publication artifact).
- Prior patches (historical records only, NOT the publication stack):
  `~/workspace/your_files/zmbor-cumulative-phase18-gs-stage2-adapter.patch`,
  `zmbor-cumulative-phase19-ghidra-oracle.patch`,
  `zmbor-cumulative-phase20-sccache.patch`.

— End of Gate A report. No code was changed, no branch was created, no push was made,
no ledger line was written. —
