# Zmbor Stabilization — Cross-Platform Verification (Gate G, final integration review)

**Date:** 2026-11-04 (Asia/Riyadh). **Scope:** what was verified on Linux
by Gates B–G, what is UNVERIFIED on MSVC and Clang and why, and the
ready-to-run publication procedures that close those items. This document
is a review record staged outside the repo tree; it is not a tree change
and does not alter any component status.

**Evidence vocabulary:** VERIFIED = actually run and checked on the stated
toolchain; UNVERIFIED = not run here, with the exact blocker and the
procedure that would close it; UNKNOWN = not established either way.
**No Linux result in this document is presented as a Windows or Clang
result.** That distinction is the whole point of this file.

## 1. What was verified on Linux (exact toolchains, commands, counts)

**Toolchains that ran.** GCC 13.3.0 (`g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1)`),
CMake 3.28.3, `-G "Unix Makefiles"` (Ninja is not installed on this VM).
Dependency cache `~/workspace/zmbor-deps/` with `FETCHCONTENT_SOURCE_DIR_*`
overrides; `-DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF
-DPS2X_ENABLE_DEBUG_UI=OFF`. Build dirs: `~/workspace/zmbor-build`
(Release), `~/workspace/zmbor-build-asan` (RelWithDebInfo,
`-DPS2X_SANITIZE=address,undefined`), `~/workspace/zmbor-iop-build` /
`~/workspace/zmbor-iop-build-asan` (standalone IOP suites; the ASan one
configured manually with `-fsanitize=address,undefined
-fno-sanitize-recover=all`). The `ps2x_tests` binary lives at
`<build>/ps2xTest/ps2x_tests`, not the build root. E2e CTests run under
`xvfb-run -a` (they fail headless for lack of DISPLAY, which is an
environment fact, not a code failure).

**Suite counts by gate (both modes unless stated).**

| Gate | `ps2x_tests` Release | `ps2x_tests` ASan+UBSan | CTest (`xvfb-run -a`) | IOP standalone |
|---|---|---|---|---|
| B (Windows headers) | 651/651 | 651/651, 0 sanitizer lines | 56 total (55 passed / 1 skipped z3 / 0 failed) both modes | 4/4 both modes |
| C (runtime/recompiler) | 661/661 | 661/661, 0 sanitizer lines | 56 total both modes | 4/4 both modes |
| D (Ghidra loader) | 672/672 | 672/672, 0 sanitizer lines | 56 total both modes | 4/4 both modes (prebuilt Gate C binaries; Gate D touched no IOP file) |
| E (GS contracts) | 683/683 | 683/683, 0 sanitizer lines | 56 total both modes | 4/4 both modes (prebuilt Gate C binaries; Gate E touches no IOP file) |
| F (verification tooling) | 683/683 | 683/683, 0 sanitizer lines (`asan-run.log`, 1,422 lines, 0 matches for `sanitizer\|runtime error\|ERROR: AddressSanitizer\|ERROR: UndefinedBehaviorSanitizer`) | 56 total both modes | 4/4 both modes (rebuilt with S12) |
| G (this review) | 683/683 on the validated forward-stack tree (see §4) | 683/683 on the validated forward-stack tree | 56 total both modes on the validated tree | 4/4 both modes from the validated tree |

**Explicit e2e confirmations (not just CTest totals).** `e2e_gs_sprite`
(exact full-frame RGB comparison, tolerance 0, 640×448) and `e2e_stripped`
explicitly Passed in both modes at Gates E, F, and G. `e2e_gs_sprite`
under ASan runs ~26.5 s standalone (Gate F measurement), against the
60 s `run_e2e.cmake` timeout — see the contention note in §5.

**Header self-containment (Gate B mechanism, Gate F wiring).**
`tools/check_public_headers.py`: per-header standalone compilation plus
the curated direct-include audit. GCC results: pre-fix 71 headers,
PASS=60 FAIL=11 (mode-1 failures exactly the S2 three; mode-2 flags
`ps2_memory.h` + 8 `<cstddef>` headers); post-fix PASS=71 FAIL=0 in both
dependency layouts. Gate G re-ran it locally against the dependency
cache: **71/71 PASS under g++**. The CI wiring (Gate F) runs it on both
Linux legs post-configure with `--deps build/_deps` and `CXX` exported
per leg (`g++`/`clang++`; `CXX=clang++` on the sanitizers leg).

**ps2autotests reproduction (Gate F, Linux only).** Corpus: anonymous
`--depth 1` clone of `unknownbrackets/ps2autotests` at
`97469ffbed8631277b94e28d01dabd702aa97ef3`; `tests/` holds exactly 55
`.elf` files. `hwtest.py prepare` → 0 RECOMPILE_FAIL; configure
`-DPS2X_HWTEST_GENERATED`; `ctest -L hwtest`; `hwtest.py report`. No
`.expected` file modified. Result: **EE 53/55 programs, 8202/8250 lines,
0 crashes, 0 timeouts, 0 recompile failures; IOP 23/23 programs,
1306/1306 lines, 0 mismatches.** Known failures unchanged and unfixed:
`cpu_ee_fpu_sqrt` 59/66 (RSQRT.S), `vu_lower_efu` 182/223 (EFU last
bits) — both hardware-probe-blocked, unskipped. Caveat (Gate F):
the reproduction used RelWithDebInfo, not Release (the first Release
hwtest build was abandoned — LTO links too slow, 9/55 after ~11 min);
`-DNDEBUG` is present in both build types and generated sources are
`-O0` either way with exact text output, but a future Release-built
reproduction would remove even that caveat.

**What Linux verification does NOT cover.** It does not cover MSVC
header graphs, MSVC diagnostics, Clang diagnostics, Windows runtime
behavior, or CI-runner behavior (Mesa/llvmpipe versions, shared-runner
load). Each of those is listed as UNVERIFIED below with its procedure.

## 2. What is UNVERIFIED on Clang (blocker + procedure)

**Status: UNVERIFIED.** `which clang` / `which clang++` return nothing
on this VM — re-checked by Gates B, C, D, E, F, and G. No Clang
standalone header result, no Clang build, and no local Clang leg of
the header check was ever run here.

**What CI covers (not a local claim).** The published workflow's
`linux-gcc-clang` job has a clang matrix leg (Ninja, Release,
`-msse4.1`), `linux-clang-sanitizers` is clang-only (ASan+UBSan,
`halt_on_error`, Z3 proofs, e2e), and `linux-clang-fuzz-smoke` is
clang-only. Those jobs will exercise Clang when the forward stack runs
in CI — but no CI run of the forward stack has happened yet (no push
has been made by any gate), so **no Clang result exists for this stack
anywhere**, local or CI.

**Procedure to close it (publication run, via the user's GitHub
connection).**

1. Push the forward stack (`zmbor-forward-50b7830-gateF.patch`, base
   50b7830) to a review branch — never to main, never force-push.
2. Run workflow `.github/workflows/build.yml` on that branch.
3. Required Clang evidence: the `linux-clang` matrix leg (Build step
   succeeds, Tests step passes), the `linux-clang-asan-ubsan` job
   (Tests under ASan+UBSan pass, e2e step passes), and the header-check
   step on both legs (exits 0; the script runs with `CXX=clang++`).
4. Pass criteria: all three legs green with no new diagnostics
   attributable to the forward stack. Any Clang-only diagnostic in a
   forward-stack file is a new finding for the owning gate's scope, not
   a reason to add the header to an exclusion list.

**Local alternative (no CI).** On any host with `clang++` installed,
run the header check directly:
`CXX=clang++ python3 tools/check_public_headers.py --repo . --deps <deps>`
with `<deps>` set to the dependency cache or a post-configure `_deps`
dir, and build the tree with `CC=clang CXX=clang++` using the same
configure flags as §1. Record the exact `clang++ --version` with the
result; a version-less "Clang passes" claim is not evidence.

## 3. What is UNVERIFIED on MSVC (blocker + procedure)

**Status: UNVERIFIED.** No MSVC toolchain exists on this VM, and
anonymous job-log access returns **403 "Must have admin rights to
Repository"**, so the verbatim diagnostics from the last main run
(41668778457, `windows-msvc-x86_64`, Build step failure) could never
be re-fetched. Gate A confirmed the failure at job level only; the
quoted C2039/C3861/C2512 lines at `ps2_memory.h` are themselves
unverified as verbatim log lines. The Gate B fix addresses the exact
code defect behind them (missing `<string>`), but **the fix's effect
on MSVC is unproven until CI runs**.

**Binding reclassification rule (Gate B, carried through Gates C–G).**
C2512 is classified as a secondary cascade of the ill-formed base
initializer, not an independent defect. **If C2512 survives a `<string>`
fix in CI, it must be reclassified as an INDEPENDENT defect** and
investigated on its own merits. The cascade theory is not assumed; it
is the hypothesis the CI run tests.

**Ready-to-run procedure (via the user's GitHub connection only).**

1. Push the forward stack (`zmbor-forward-50b7830-gateF.patch`, base
   50b7830) to a review branch — never to main, never force-push.
2. Run workflow `.github/workflows/build.yml`, job
   `windows-msvc-x86_64`, on that branch.
3. Grep the Build step log for `C2039`, `C3861`, `C2512` at
   `ps2_memory.h`.
4. Pass criteria: the Build step succeeds, the full job completes, and
   none of those three diagnostics appears at `ps2_memory.h`.
5. Apply the reclassification rule above to whatever the log shows.
6. Header-check MSVC adaptation (UNVERIFIED until a Windows runner
   tries it): the script uses GCC/Clang-style flags, so the Windows leg
   is NOT wired. Try `clang-cl` with the Linux invocation first; if
   that fails, the script needs an MSVC flag mode (`/std:c++20`, `/Zs`).
   Do not claim an MSVC header-check result until one of those two
   paths has actually run.

**What the Windows job does and does not cover (by documented design).**
The Windows job runs unit tests, fpenv self-tests, and the VU1 bench.
It has **no e2e step** because hosted Windows runners have no OpenGL
(WGL) driver and the runtime opens a window even when hidden. The e2e
programs run on the Linux jobs (xvfb + Mesa) and in the local Windows
gate. A green Windows job therefore never, by itself, evidences e2e
behavior.

## 4. Gate G final validation on the forward-stack tree (Linux, GCC)

Gate G re-executed the Gate A §2.4 procedure end to end rather than
inheriting prior gates' validation:

1. Fresh anonymous clone of `https://github.com/m7kor/Zmbor`, checked
   out at exactly `50b78304b627236937887737d9ed4b5eebebf1b8`
   (verified via `git rev-parse HEAD`).
2. `git apply --check` on `zmbor-forward-50b7830-gateF.patch`: clean.
   Applied clean.
3. Recursive tree comparison vs the working tree (excluding `.git`,
   `mc0`, `mc1`, `__pycache__`, `*.pyc`): **zero content differences
   except the intended ones** — `README.md` differs (working tree's
   older version vs HEAD's newer version; the forward patch deliberately
   excludes README, so the patched clone correctly keeps HEAD's), and
   HEAD-only files are present only in the validation clone
   (`docs/architecture/NEXT_GENERATION_RECOMPILER_DESIGN.md`,
   `docs/research/`, `research/poc1`, `research/poc2`, `research/poc3`).
4. Negative checks verified explicitly in the validation clone:
   `git diff HEAD --stat` on `README.md`, `docs/research`,
   `docs/architecture/NEXT_GENERATION_RECOMPILER_DESIGN.md`,
   `research/` is empty, and `git status` on those paths is clean.
5. Test gates on the validated tree (same configure as §1, sccache on
   PATH reusing the Phase 20 warm cache): `ps2x_tests` Release and
   ASan+UBSan, full CTest under `xvfb-run -a` both modes, IOP standalone
   4/4 both modes, `e2e_gs_sprite` and `e2e_stripped` explicitly both
   modes. Exact counts are in the final report; the validation build
   required one environment repair first (see §5).

## 5. Environment incidents that bound these claims (owned, resolved)

- **VM replacement wiped X11/GL dev packages (Gates E, F, G).** Each
  time, the symptom was a configure or load failure, not a code
  failure: Gate F's ASan binary failed to load (`libOpenGL.so.0` not
  found) while Release passed; Gate G's validation-clone configure
  failed in GLFW ("RandR headers not found"). Each time the fix was a
  temp-only apt fetch (single mirror, main + noble-updates, temp
  lists/cache under scratch, system sources untouched) restoring
  `libopengl0` / `libgl1-mesa-dev` / `libxrandr-dev` / `libxinerama-dev`
  / `libxcursor-dev` / `libxi-dev` (Gate G also added `libx11-dev`).
  Lesson recorded in the ledger: **check shared-library loadability
  (`ldd`) and header presence after any VM replacement before counting
  a binary run or a configure failure.** Gate G verified `ldd` clean
  on both main binaries before counting them, and re-ran the validation
  configure only after the header restore.
- **`/tmp` is a 512 MB tmpfs (Gate E).** Temp apt state filled it and
  broke an LTO link. Patch clones and large temp state live in
  `~/workspace` scratch, never `/tmp`, since Gate E.
- **Header edited while a build ran (Gate E).** Mixing two `GS` class
  layouts produced a deterministic ASan crash that looked like a
  product bug; fresh-build bisects proved it. Lesson: never edit a
  header while a build runs. Gate G edited nothing in the tree.
- **ASan `gs_sprite` contention flake (Gate F, investigated, not
  hidden, not softened).** First full ASan CTest was 55/56 with
  `e2e_gs_sprite` timing out at 60.38 s vs the 60 s `run_e2e.cmake`
  timeout while a `--parallel 4` hwtest build ran concurrently on the
  4-core VM. Standalone ASan re-run passed (26.95 s); full ASan CTest
  re-run passed 56/56. Classified as contention, not regression; the
  60 s timeout was NOT changed. CI note: ASan `gs_sprite` runs ~26.5 s
  standalone, so the timeout has headroom unloaded, but a loaded shared
  runner could flake. This is a CI-flake risk to watch on the first
  publication run, not a product defect.
- **0-byte objects after interrupted builds (recurring pitfall).** A
  build killed mid-link can leave a 0-byte object that `make` treats as
  up-to-date (seen in Phases 10b/18, Gate B, and Gate E's ASan
  incident). Any mysterious failure after an interrupted build must
  start with a 0-byte-object scan, not a code investigation.

## 6. What would change this document

- A CI run of the forward stack on a review branch (Clang legs +
  Windows job green, or diagnostics with the reclassification rule
  applied) moves §2/§3 items from UNVERIFIED to VERIFIED with the run
  URL and job conclusions recorded.
- A local Clang run (header check + build, with version recorded) moves
  the local Clang leg to VERIFIED without waiting for CI.
- An MSVC header-check adaptation that actually runs (clang-cl or an
  MSVC flag mode) moves that item to VERIFIED.
- Until at least the CI run exists, any claim that the stack is
  "verified cross-platform" is false, and the final verdict must say so.
