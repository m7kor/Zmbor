# Zmbor Stabilization — Build / Performance Evidence (Gate G, final integration review)

**Date:** 2026-11-04 (Asia/Riyadh). **Scope:** what was actually
measured about build performance (Phase 20 sccache), what those
numbers do and do not prove, the Gate F hwtest build-type caveat, the
ASan `gs_sprite` contention note and its timeout implication, and what
was NOT measured. This document is a review record staged outside the
repo tree; it is not a tree change and does not alter any component
status.

**Standing rule for this document.** Every performance number below
carries its conditions (machine, toolchain, build type, parallelism,
what the wall time includes and excludes). A number quoted without its
conditions is not evidence. **No new performance claim is made by Gate
G** — this document consolidates prior measurements and states their
limits.

## 1. Phase 20 sccache — the measured result (VERIFIED, with conditions)

**What was installed.** sccache 0.19.0 (Mozilla, Apache-2.0, Linux
x86_64 musl portable binary, SHA-256 verified against the published
sidecar) at `~/workspace/zmbor-tools/sccache-v0.19.0/sccache` (under
`~/workspace` so VM replacements cannot wipe it); cache at
`~/workspace/zmbor-sccache-cache`, 10 GiB cap via `SCCACHE_CACHE_SIZE`,
local disk only, no credentials.

**The correction that made the measurement meaningful.** The pre-Phase-20
hook (option `PS2X_ENABLE_SCCACHE`, default ON, in
`ps2xRuntime/CMakeLists.txt`) was directory-scoped and wrapped only
**79/291 real compile rules (27%)** — installing the binary alone
would have left ~73% uncached. Phase 20 moved the launcher block to the
root `CMakeLists.txt` after `project()` and before any
`add_subdirectory()`, removed the redundant block from ps2xRuntime,
and re-configure proved **279/279 rules wrapped**. Configuring without
sccache prints "not found" once and leaves zero launcher references.

**Measurement traps recorded (so later readers do not re-fall into
them).** Grepping `compile_commands.json` matched the build dir's own
name (`zmbor-build-sccache`) in `-I` flags — it omits the launcher;
`build.make` rules are authoritative. The "4 unwrapped rules" were
CMake's internal LTO test programs, not real rules.

**Numbers (like-for-like: same VM, GCC 13.3.0, CMake 3.28.3,
`--parallel 4`, Release, same build dir, stats zeroed, walls exclude
~8 s configure).**

| Build | Wall | Notes |
|---|---|---|
| Cold (cache empty) | 557 s | — |
| Warm (cache populated) | 205 s | 278/278 hits, 100%, 0 errors, cache 56 MiB |
| Speedup | 2.7× | Warm floor is LTO linking (uncached) + codegen + build-system floor |

**Correctness of the cached build.** The sccache-built binary passed
`ps2x_tests` 650/650 (Phase 20 count), CTest 56 including all 5 e2e;
IOP prebuilt binaries passed as regression sanity. ASan+UBSan was NOT
re-run for Phase 20 (launcher-only change; Phase 19 ASan 650/650
stands) — stated here so it is not mistaken for a Phase 20 ASan claim.

**What these numbers do NOT prove.**

- The cold 557 s is NOT comparable to POC 3's 874.8 s full-build figure
  (different conditions, different tree state). Do not quote a
  "874.8 → 205 s" improvement — it never happened under one protocol.
- The ccache 5.9× figure from POC 3 is a different tool's subset
  number (cold 134.0 s vs warm 22.8 s on the `ps2_recomp` subset only).
  It is not an sccache result and must not be quoted as one.
- Single-TU warm time was not measured. Peak build memory is UNKNOWN
  and must not be quoted.
- The cache is local-disk only: a fresh machine starts cold. Compiler
  upgrades invalidate cache entries.
- The standalone IOP configure does not pick up the root launcher
  (documented Phase 20 limitation). IOP builds are not sccache-warmed
  by the root block.
- Generation time is negligible either way (POC 3: 0.036–0.101
  s/program) — compiling + LTO-linking is the bottleneck, not code
  generation. Any future "faster pipeline" claim must address
  compile/link, not generation.

## 2. What Gate G's validation builds add (and do not add)

Gate G's validation-clone builds (Release + ASan+UBSan, same configure
as the main builds, sccache on PATH reusing the Phase 20 warm cache)
are **correctness validation of the forward stack, not performance
measurements**. Their wall times were not recorded under a measurement
protocol (parallel background work, shared VM) and are not quoted here.
The only performance-relevant observation from Gate G's builds: the
Release validation build completed and produced a working
`ps2x_tests` binary, and the ASan validation build completed under the
same conditions — i.e., the forward stack builds cleanly on a fresh
clone with a warm cache. That is a build-health statement, not a speed
claim.

## 3. Gate F hwtest build-type caveat (carried, with its exact scope)

The Gate F ps2autotests reproduction (EE 53/55, 8202/8250 lines; IOP
23/23, 1306/1306 lines; 0 crashes/timeouts/recompile failures) used
**RelWithDebInfo**, not Release, because the first Release hwtest build
was abandoned (LTO links too slow: 9/55 programs after ~11 min).
Why the caveat is small but real:

- `-DNDEBUG` is present in both build types, generated sources compile
  `-O0` either way, and hwtest compares exact text output — so the
  build type should not change line-matching outcomes.
- "Should not" is not "cannot": optimization-level differences in the
  runtime could in principle change a timing-sensitive or
  floating-point-sensitive output line. No such line was observed to
  differ, but the reproduction does not prove Release would match.
- **Action for a future run:** a Release-built reproduction removes
  even that caveat. Until then, quote the reproduction as
  "RelWithDebInfo reproduction, zero delta vs the historical baseline"
  — never as a bare "baseline reproduced" without the build type.

The same LTO-link cost explains why hwtest is not a quick gate: any
future CI gate over hwtest must budget link time (or use
RelWithDebInfo deliberately, with the caveat recorded).

## 4. ASan `gs_sprite` contention note and the timeout implication

**Observation (Gate F, investigated, not hidden, not softened).**
First full ASan CTest: 55/56, with `e2e_gs_sprite` timing out at
60.38 s vs the 60 s `run_e2e.cmake` timeout, while a `--parallel 4`
hwtest build ran concurrently on the 4-core VM. Standalone ASan re-run
of `gs_sprite|stripped` passed (26.95 s); full ASan CTest re-run passed
56/56. Classified as contention, not regression. The 60 s timeout was
NOT changed.

**Gate F corroboration (main-agent re-run).** The main agent's
independent ASan CTest re-run at Gate F passed 56/56 with
`e2e_gs_sprite` at 25.91 s. Unloaded, the timeout has ~2.3× headroom
over the standalone time (~26.5–26.95 s).

**Implication (stated, not fixed).** On a loaded shared CI runner,
ASan `gs_sprite` could flake the same way. The timeout was
deliberately left at 60 s (changing it to hide contention would be
softening a gate). If the first publication CI run flakes exactly
here, the correct response is to re-run and to record runner load —
not to raise the timeout silently and not to mark the suite red
without the contention check. Any future timeout change needs its own
justification (measured loaded-runner times), not a flake.

**Related timing facts (for CI budgeting, not claims).**
`ps2x_tests`: ~20 s Release, ~65 s ASan (main-tree re-runs at earlier
gates). Full ASan CTest wall time on this VM was not recorded under a
measurement protocol at Gate G; do not quote one.
CPU GS-only measurements from the GS assessment harness (for context,
not gates): sprite-equivalent ~8.0 ms/frame; 1,000 sprites ~27.6–33.1
ms; 1,000 triangles ~33.4–34.6 ms; present ~2.1 ms floor. These are
CPU-backend harness numbers on this VM, not GPU claims and not
regression thresholds.

## 5. Lavapipe / Vulkan timing (Phase 18 E2e — context only, never gated)

E2e throughput was timing-only, never gated (Phase 18): lavapipe CPU
device small scenes showed PGS draw ~48–156× CPU (~320–350 ms fixed
per-frame overhead regardless of scene size), F-large 9.8×. Vulkan
stays opt-in; lavapipe is a CPU device, so **no GPU performance claim
of any kind follows**. These numbers are recorded here only to prevent
their reuse as performance evidence: they measure a software Vulkan
implementation's fixed overhead, not the adapter's GPU performance.

## 6. Environment costs that are not product performance

- **VM replacements** (Gates E, F, G) wipe `/tmp`, apt packages, and
  anything outside `~`. Each wipe costs a restore cycle (temp-only apt
  fetch, rebuilds) and can masquerade as a product failure (missing
  headers → configure failure; missing runtime lib → load failure).
  Gate G's validation required one such restore (X11/GL dev headers)
  before the fresh-clone configure could succeed.
- **`/tmp` (512 MB tmpfs).** Temp apt state filled it during Gate E
  and broke an LTO link. Large temp state lives in `~/workspace`
  scratch now.
- **Interrupted builds.** A killed build can leave a 0-byte object
  that `make` treats as up-to-date, producing confusing downstream
  failures (seen in Phases 10b/18, Gate B, Gate E). The cost is
  diagnostic time, not product time — but it recurs, so the scan is
  part of the build procedure now.
- **Header edited while a build ran (Gate E).** Mixed class layouts
  produced a deterministic crash; the diagnostic cost was a full
  bisect series (Gate D tree, product-only, full Gate E) before the
  cause was owned. Lesson: never edit a header while a build runs.

## 7. What was NOT measured (explicit non-claims)

- No generated-program runtime performance was measured in the
  stabilization program (no game, no benchmark title, no profiling
  run). The core-mission priority "profile generated programs and
  optimize real bottlenecks" remains future work with a real workload.
- No PGS-on-GPU performance was measured (lavapipe only; see §5).
- No single-TU compile times, no peak build memory, no link-time
  breakdown beyond "LTO linking is the warm floor" (Phase 20, qualitative).
- No CI-runner timings exist (no CI run of this stack has happened).
- No comparison between GCC and Clang build times or outputs exists
  (Clang absent on this VM).
- The sccache warm floor (205 s) is not a target and not a regression
  threshold; it is a measurement of one configuration on one VM.
