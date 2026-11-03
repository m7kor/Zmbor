# POC 3 results — Zmbor baseline measurement (2026-11-03)

Source of truth: raw logs under `~/workspace/zmbor-research/poc3/results/`.
This file digests them; where it disagrees with a log, the log wins.
Status vocabulary: IMPLEMENTED / TESTED / VERIFIED / UNVERIFIED / UNKNOWN
(see the Category E section file for candidate verdicts).

## 0. Environment and load caveat

- Host: Ubuntu 24.04, GCC/G++ 13.3.0, CMake 3.28.3, GNU Make, 4 CPUs,
  7.7 GiB RAM. X11/GL dev headers present. No Clang, no Ninja, no sccache,
  no `/usr/bin/time`, no `bc` at start. `ccache` 4.9.1 installed via apt
  during POC 3 for the cache experiment only.
- Reference clone: `~/workspace/zmbor-research/main`, HEAD
  `a1bb50be3c797025d96e5c744e615c632b0a8f83` (2026-11-03 14:08:55 +0300),
  `git status` clean before and after POC 3 — the clone was never written.
- LOAD CAVEAT: a separate Phase 15 coordinator build/test workload shared
  the 4 cores during much of POC 3. All wall times are "as measured under
  shared load". Treat absolute seconds as order-of-magnitude, and ratios
  measured back-to-back (ccache cold/warm, PCH on/off) as more reliable
  than cross-session comparisons.

## 1. Configure

| Run | Time | Log |
|---|---|---|
| Main build, first configure (CMake's own "Configuring done") | 44.9 s | `results/configure.log` |
| Main build, first configure (outer wall incl. script) | 45.5 s | `results/configure.summary.txt` (WALL_S blank: `bc` missing at the time; CMake's 44.9 s is the citable figure) |
| ccache-experiment cold configure (fresh dir, ccache launcher) | 48.6 s | `results/ccache-experiment.summary.txt` |
| ccache-experiment warm configure (fresh dir, cache warm) | 11.1 s | same |
| IOP standalone configure | 0.7 s | `results/iop-configure.log` |

Configure notes: sccache reported "not found; continuing without compiler
launcher" (expected — the hook in `ps2xRuntime/CMakeLists.txt` is a silent
no-op without the binary). Dependency overrides resolved from
`~/workspace/zmbor-deps/` (log shows nlohmann_json used from the cache).
FFmpeg disabled → "MPEG video decode falls back to stub frames".

## 2. Full build (main-build, Release, `-j4`)

- Wall: **874.8 s** (14.6 min), exit code 0. Log: `results/build.log`,
  summary: `results/build.summary.txt`.
- Build dir total afterwards: **303 MB**.
- Peak memory: **UNKNOWN (isolated)**. The script's sampler reported
  2,490,680 KB system-wide compiler RSS, but that sum included the
  concurrent Phase 15 build's compiler processes. Do NOT quote 2.49 GB as
  Zmbor's peak. Credible method for a re-run: idle machine +
  `/usr/bin/time -v cmake --build …` or cgroup peak accounting.

## 3. Incremental rebuilds (build dir only)

| Probe | Time | What actually happened |
|---|---|---|
| Delete one object (`ps2_recomp_lib/.../function_emitter.cpp.o`), rebuild target `ps2_recomp` | **53.7 s** | Recompile of one large recompiler TU + static archive + link. Log: `results/incremental.log` |
| "No-op" full rebuild immediately after | **244.5 s** | NOT a true no-op: the rebuilt `ps2_recomp` binary re-triggered all 4 e2e custom commands (assemble + recompile) → 8 generated TUs recompiled + 4 LTO links. This cascade cost is itself a finding: any recompiler change costs ~4 min of e2e rebuild even for 11–51 KB generated files. Log: `results/incremental-noop.log` |

## 4. Binary sizes (Release, bytes)

| Binary | Bytes | Path in main-build |
|---|---|---|
| `ps2_recomp` | 2,217,736 | `ps2xRecomp/ps2_recomp` |
| `ps2x_e2e_basic` | 3,264,408 | `ps2xTest/e2e/ps2x_e2e_basic` |
| `ps2x_e2e_gs_sprite` | 3,255,544 | `ps2xTest/e2e/ps2x_e2e_gs_sprite` |
| `ps2x_e2e_branchdelay` | 3,261,008 | `ps2xTest/e2e/ps2x_e2e_branchdelay` |
| `ps2x_e2e_vu_macro` | 3,281,320 | `ps2xTest/e2e/ps2x_e2e_vu_macro` |
| `ps2x_tests` | 8,315,824 | `ps2xTest/ps2x_tests` |
| `ps2x_bench_vu_fmac` | 51,072 | `ps2xTest/bench/ps2x_bench_vu_fmac` |
| `ps2x_bench_vu1` | 374,920 | `ps2xTest/bench/ps2x_bench_vu1` |

Generated sources (single TU per program — e2e uses `single_file_output = true`):

| Program | `ps2_recompiled_functions.cpp` | Guest ELF |
|---|---|---|
| basic | 29,269 B | 5,668 B |
| gs_sprite | 11,294 B | 5,364 B |
| branchdelay | 51,208 B | 5,984 B |
| vu_macro | 45,009 B | 6,080 B |

## 5. Test suites — exactly what ran and what did not

| Suite | Result | Notes |
|---|---|---|
| `ps2x_tests` binary, run directly (NOT a CTest test — main registers no `add_test` for it) | **620/620 passed, 0 failed, 56.8 s** | Log: `results/ps2x_tests.log`. 620 matches main (not 633: the +13 come from unpublished Phases 12–14, absent here). |
| Full CTest (`xvfb-run -a ctest`) | **54 passed / 1 skipped / 0 failed out of 55, 38.0 s wall** | Log: `results/ctest-full.log`. Skipped: `z3_int_semantics` (z3-solver absent → skip path, exit 77 by design). Composition: 3 research selftests + 45 gs_cache + 4 e2e + 2 fpenv + 1 z3 (skipped). |
| e2e CTests alone | **4/4 passed** | basic 1.29 s, gs_sprite 10.02 s, branchdelay 1.01 s, vu_macro 0.87 s in the full-ctest run (an earlier standalone e2e run gave 1.50 / 8.31 / 0.97 / 0.62 s — run-to-run variance under load; both runs passed). Log: `results/e2e.log`. |
| e2e binaries run directly under `xvfb-run -a` | **4/4, rc=0**, expected lines matched | basic 1.32 s (`E2E sum=5050 fact=3628800 b63=1 lq=1`), branchdelay 1.30 s, vu_macro 1.70 s, gs_sprite 6.79 s (`E2E gs frame submitted`). These times are dominated by runtime startup + the frame loop, NOT guest CPU — they are correctness runs, not CPU benchmarks. Stdouts: `results/e2e-*.stdout.txt`. |
| IOP standalone suite (separate configure, `ps2xIOP` as its own project, `-DPS2X_IOP_BUILD_TESTS=ON`) | **4/4 passed, 0.31 s test time** (build 60.2 s) | `ps2_iop_emulator_tests`, `ps2_iop_import_tests`, `ps2_iop_compatibility_tests`, `ps2_iop_import_version_tests`. NOT part of the top-level main build (option defaults OFF) — run separately in `poc3/iop-build/`. Log: `results/iop-build.log`. |
| ps2autotests ladder / hwtest suite | NOT RUN | Needs `PS2X_HWTEST_GENERATED` prepared dir (third-party data, not in repo). Not claimed. |
| ASan+UBSan build | NOT RUN | Previous phases report it on other machines; not re-run here (one Release build was the POC 3 scope). |

## 6. C++ generation time (ps2_recomp, externally wall-clocked)

`ps2_recomp` prints no timing, so each e2e `.toml` was re-run verbatim and
wall-clocked (output regenerated in place in main-build; content
identical to the build's own generation). Log: `results/gen.log`.

| Program | Generation wall time |
|---|---|
| basic | 0.101 s |
| branchdelay | 0.091 s |
| vu_macro | 0.036 s |
| gs_sprite | 0.044 s |

Finding: generation itself is negligible (<0.11 s) for the micro-corpus;
the cost is downstream (compiling + LTO-linking the generated code), not
in `ps2_recomp`. Generation time for a real game is UNKNOWN and must be
measured on a real ELF, not extrapolated from 5–6 KB ELFs.

## 7. Production flags for generated code (GCC, from main-build `compile_commands.json`)

Generated e2e TU compile command (branchdelay):

```text
/usr/bin/c++ … -O3 -DNDEBUG -std=gnu++20 -flto=auto -fno-fat-lto-objects
  -ffp-contract=off -msse4.1 -c …/ps2_recompiled_functions.cpp
```

So Release generated code ALREADY gets full GCC LTO (`-flto=auto`,
thin objects only — no fat objects). Any LTO proposal must beat this
baseline, not "add LTO" to a non-LTO build. MSVC ReleaseMode differs
(`/O2 /Ob2 /Oi /GL /fp:fast /arch:AVX2` + `/LTCG`); the GCC/MSVC flag
parity gap (AVX2 vs SSE4.1, `/fp:fast` vs no fast-math) is an open
correctness-parity question, guarded in-tree by the fpenv tests (both
passed here) — not a free performance lever, because `-ffast-math` is
refused at compile time for code including the arithmetic headers
(`tools/fpenv/CMakeLists.txt`, `fpenv_fastmath_rejected` test).

## 8. ccache experiment (target `ps2_recomp` only, 117 compilations)

Procedure: `poc3/ccache_experiment.sh`; isolated cache `poc3/ccache-dir/`;
fresh build dir for each leg; same configure flags as §0 plus
`-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`.

| Leg | Configure | Build `ps2_recomp` | ccache stats |
|---|---|---|---|
| Cold (empty cache) | 48.6 s | **134.0 s** | 0 hits / 117 misses |
| Warm (cache from cold leg) | 11.1 s | **22.8 s** | 117 direct hits (cumulative 117/234) |

Warm/cold ratio for this target subset: **5.9×** (saves ~111 s).
Scope warning: this is the `ps2_recomp` target subset (fmt + rabbitizer +
libdwarf + recomp lib + recomp binary), NOT the full build. A full-build
warm/cold number was not measured (second full build ≈ 15 min under
load). The warm binary was smoke-checked (`ps2_recomp` with no args
prints usage, rc as expected). Stats: `results/ccache-stats-cold.txt`,
`results/ccache-stats-warm.txt`.

## 9. Generated-TU compile experiment (compile-only, single runs, noisy)

Manual `g++` compiles of generated TUs with production include paths
(full flags minus the LTO link step). Summary: `results/tu-experiment.summary.txt`.

| TU (source bytes) | -O0 | -O2 | -O3 | -O3 -flto (compile-only) |
|---|---|---|---|---|
| branchdelay (51,208) | 9.28 s | 9.23 s | 12.07 s | 13.82 s |
| vu_macro (45,009) | 8.41 s | 18.53 s | 15.01 s | 6.21 s |
| basic (29,269) | 6.13 s | 8.31 s | 7.21 s | 6.42 s |

Repeat (cleaner moment): branchdelay -O0 **6.03 s** vs -O3 **6.11 s**.
Object sizes: branchdelay -O0 45,576 B vs -O2/-O3 ~15,060 B vs LTO
103,872 B (bytecode); vu_macro similar pattern.

Reading (do NOT over-read single noisy runs): -O level does not dominate
TU compile time — a 51 KB TU costs ~6 s even at -O0, and repeats move by
~50%. The cost floor points at parsing the heavy runtime headers
(`ps2_runtime.h`, `ps2_syscalls.h`, `ps2_stubs.h`, `ps2_runtime_macros.h`),
not at optimizer effort on generated code. That is what motivates the
PCH prototype (§10), and it falsifies "just raise/lower -O" as a lever
for generated-code build time. A decision-grade version of this
experiment needs ≥5 runs per cell on an idle machine (design in the
section file).

## 10. PCH prototype (isolated; no clone changes)

Prototype header `poc3/pch-test/pch.h` = the stable runtime headers every
generated TU includes (per-program generated header deliberately
excluded). Built with production flags/defines.

- PCH build: **44.2 s, 129 MB `.gch`**, clean stderr.
- branchdelay TU with PCH: **3.31 s** (object 15,056 B — identical size to
  the non-PCH -O3 object), vs **6.03–6.11 s** without PCH (repeat runs).
- PCH use verified with `-H`: `! …/pch.h.gch` (the `!` marks PCH use).
- Break-even: ≈ 44 / (6.1 − 3.3) ≈ **16 TUs per PCH rebuild**. A PCH must
  be rebuilt whenever any included header changes, must match flags
  exactly, and MSVC PCH semantics differ — Windows parity must be tested,
  not assumed.

## 11. Existing micro-benchmarks (this machine, GCC Release, shared VM)

`ps2x_bench_vu_fmac` (default args; 31 runs over 65,536 vectors):

| Op | legacy median | lanes median | sse2 median |
|---|---|---|---|
| add (normal) | 1.71 ns | 85.41 ns | 16.28 ns |
| sub (normal) | 1.59 ns | 81.16 ns | 12.20 ns |
| mul (normal) | 5.92 ns | 22.92 ns | 7.08 ns |
| add (random bits) | 2.68 ns | 100.97 ns | 15.75 ns |
| sub (random bits) | 3.07 ns | 138.93 ns | 35.11 ns |
| mul (random bits) | 207.34 ns | 19.53 ns | 8.60 ns |

Max values reached ~900–1200 ns (shared-VM contention). Per the repo's own
benchmark methodology, differences smaller than the min..max spread are
not claims; these medians are reported as "ran on this machine", NOT
compared against the Phase 5 Windows/MSVC numbers (different machine,
compiler, and load — any such comparison would be meaningless).

`ps2x_bench_vu1 1`: correctness ok (0 lane mismatches over 256 vertices);
steady median 512.1 µs/run [473.6..6739.2], reupload 598.6 µs/run
[569.5..6928.9], 256 vertices. Same contention caveat.

## 12. What POC 3 did NOT establish (UNKNOWN, by design)

- Generated-code runtime performance for any real game (no game ran).
- Whether graphics, VU, SPU2, or CPU emulation dominates runtime (no
  workload exists to profile; if profiling later shows graphics as the
  bottleneck, that investigation belongs to the graphics category, not
  this one).
- Compile time / link time / memory for a game-scale generated codebase
  (single-file vs partitioned, LTO link scaling) — only micro-corpus
  evidence exists; the section file's experiment designs cover the
  game-scale questions.
- Any vendor-typical speedup applied to Zmbor (none is claimed anywhere
  in this program's outputs).
