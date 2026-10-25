# Zmbor — handoff (updated 2026-10-25, Phase 5)

Official repository: **https://github.com/m7kor/Zmbor**. Zmbor is the continuation of the "PS2Recomp Ultimate" work,
built on the upstream project `ran-j/PS2Recomp@eb8688b` plus upstream #384. Re-measure before quoting any number.

## Phase 5 (Draft PR #3, not merged)

- Branch `ultimate/phase5-architectural-correctness`, worktree `D:\Project\Zmbor\wt-phase5`, based on Zmbor `main`
  `20f1a97` (PR #2 merged). `1d7629d` is the WIP start and is kept unchanged; later commits are listed in
  `CHANGELOG.md` (Phase 5). Pushed with the owner's authorization as Draft PR https://github.com/m7kor/Zmbor/pull/3 (do not mark ready or merge without approval); CI 6/6 on `d1c6759`.
- Build dirs: `build` (baseline, `20f1a97`) and `build-wip` (branch head); hardware tests use
  `D:\Project\Zmbor\hwtest_work\gen5`; reports `p5_base`, `p5_wip`, `p5_micro`, `p5_perf` (`.json/.md`) in
  `hwtest_work`. Full gate script used: scratchpad `gate5.ps1` (build, `ctest -L hwtest`, report against
  `p5_base.json`, `ps2x_tests`, `ctest -LE hwtest`, IOP ctest).
- Measured at the branch head (Windows MSVC): hardware tests 53/55 exact, 8202/8250 lines (exact-source gate on `d8ac102`), no regressions vs
  `20f1a97`; `ps2x_tests` 541/541; ctest (non-hwtest) 52/52; e2e 4/4; `ps2xIOP` 4/4. No physical PS2 and
  no ps2sdk toolchain were available: no new hardware measurement exists; every probe is `UNMEASURED`.
- Architecture documents (start here): `docs/architecture/evidence-register.md` (every model with its evidence),
  `vu-ee-arithmetic-semantics.md` (VU FMAC contract, lowerings, FP policy in section 4), `hardware-probes.md` (probe
  pipeline and inventory), `benchmark-methodology.md`, `control-flow-invariants.md`.
- Tools: `tools/semantics/` (exact models, Z3 search `z3_add_models.py` needs `pip install z3-solver==4.13.0.0`,
  RSQRT analysis `ee_rsqrt_models.py`), `tools/hwprobes/probe_lab.py` (manifests, ingest), `tools/fpenv/` (FP flag
  guard, ctest `fpenv_flag_independence`), `tools/bench/` (`ps2x_bench_vu_fmac`).
- Open questions are listed per entry in the evidence register; the main ones: EE ADD guard bits (P1), MUL.S form
  (P2), RSQRT.S datapath (P3, best fit post hoc), VU MUL asymmetry (P4), EFU cancellation residual, macro-mode
  MAC/STATUS flags (gap), FMAC overflow threshold (provisional).

## Phase 4 (merged into main as PR #2)

- Branch `ultimate/phase4-hardware-accuracy` was merged into `main` by the owner as **PR #2**
  (https://github.com/m7kor/Zmbor/pull/2, merge commit `20f1a97`). Local worktree: `D:\Project\Zmbor\wt-phase4`.
- Commits on Zmbor `main` `00173a2`:
  - `3d935f4` branch in a delay slot;
  - `a7449bb` heap ownership;
  - `f81312d` COP0 performance counters;
  - `363f8f2` MUL.S and vi24;
  - `cb77d79` docs;
  - a follow-up that removes the runtime-arena fallback (below).
- Measured on this branch (Windows MSVC): hardware tests **53 / 55 exact, 8202 / 8250 lines**, 0 crashes,
  0 timeouts, no regressions against the re-measured `00173a2` baseline (47 / 55, 8172); `ps2x_tests` 530 / 530;
  ctest 49 / 49; `ps2xIOP` 4 / 4.
- GitHub CI (Linux GCC, Linux Clang, Windows MSVC) builds and runs `ps2x_tests` only. It does not run the 55
  hardware tests, which need the ps2autotests ELFs prepared outside the repository; those numbers are local runs.
- Hardware tests for this branch use `D:\Project\Zmbor\hwtest_work\gen4` (configure with
  `-DPS2X_HWTEST_GENERATED=D:/Project/Zmbor/hwtest_work/gen4`); run reports `p4_base.json` and `p4_m1/m2/m4/m5.json`.
- Runtime arena policy (follow-up to `a7449bb`): `runtimeMalloc` returns 0 when the arena is full; it no longer falls
  back to the guest heap allocator. That fallback placed runtime blocks at the heap base, where the program's own sbrk
  starts. Every HLE caller handles 0. An ELF with a segment in 0x00020000-0x00080000 (arena or any fixed HLE pool)
  is refused before anything is written, by both `PS2Runtime::loadELF` and the SifLoadElf loader (the guest gets an
  error); none of the 55 hardware programs loads there.
- Limited evidence, kept as documented hypotheses:
  - the delay-slot model is fitted to three all-taken hardware rows;
  - vi24 rests on one row;
  - MUL.S fits 72 rows, which do not distinguish it from wider truncated-array forms.
- Remaining failures: `cpu_ee_fpu_sqrt` (RSQRT.S) and `vu_lower_efu`, see below.

## State before Phase 4

There are two revisions, measured separately:

| check | pre-integration `ae1c119` (parent `eaaa0a4`) | published Zmbor `main` (`00ac972` merge, docs on top) |
|---|---|---|
| hardware result tests (ps2autotests, real-PS2 `.expected`) | **47 / 55 exact**, 8172 / 8250 lines (99.1%), 0 crashes, 0 timeouts (measured on `eaaa0a4`) | **not rerun** after the #384 merge |
| `ps2x_tests` (unit) | 519 / 519 | 520 / 520 (#384 adds one GS test) |
| ctest in `build/ps2xTest` without `hwtest` label (research DB, GS cache, end-to-end) | 48 / 48 | 48 / 48 |
| `ps2xIOP` ctest | 4 / 4 | 4 / 4 |

GitHub Actions on Zmbor (`.github/workflows/build.yml`) builds and runs `ps2x_tests` on Linux GCC, Linux Clang and
Windows MSVC for every push and pull request.

Commercial game compatibility has **not** been verified. These results come from the hardware test ELFs, unit
tests and synthetic end-to-end programs.

Progress of the hardware matrix: first run 6/54 → end of Phase 2 32/55 → Phase 3 VU 38/55 → Phase 3 VIF/SPR/DMAC
47/55. No test that passed in an earlier run has failed in a later one.

## Branches and commits

- **Published:** `main` of https://github.com/m7kor/Zmbor. It is the integration history:
  - `ae1c119` merged with upstream #384 (`e563698`, "GIF: treat FLG=3 like IMAGE"), giving `00ac972`;
  - `9abdd93` (Zmbor README and `ROADMAP.md`);
  - this handoff correction.
- `main` is protected: pull request required, no force-push, no deletion. Work on a branch and open a PR.
- **Continue from** local worktree `D:\Project\Zmbor\wt-integration`, branch `ultimate/phase3-integration`, which
  matches Zmbor `main`. Push with gh credentials:
  `git -c credential.helper= -c "credential.helper=!'C:/Program Files/GitHub CLI/gh.exe' auth git-credential" push https://github.com/m7kor/Zmbor.git <branch>`.
- **History only:** local worktree `D:\Project\Zmbor\wt`, branch `ultimate/phase0-recomp-correctness` at `ae1c119`,
  is the pre-integration development line. It is not published on its own, and its commits are all in Zmbor `main`.
  `D:\Project\Zmbor\PS2Recomp` is the original upstream clone (`eb8688b`).
- The fork `m7kor/PS2Recomp` (main `e563698`) and upstream `ran-j/PS2Recomp` were not modified; nothing from this
  work was pushed there.
- Key commits: `0d60991` (hwtest pipeline), `934ce1d` (FPU/kernel fixes), `d045786` (VU microcode execution),
  `eaaa0a4` (VIF/SPR/DMAC). `CHANGELOG.md` lists every change by phase with its root cause.

## Build (Windows, MSVC 14.44 BuildTools + Ninja)

```
vcvars64.bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF ^
  -DPS2X_IOP_BUILD_TESTS=ON -DPS2X_HWTEST_GENERATED=D:/Project/Zmbor/hwtest_work/gen
cmake --build build --parallel 10
```

- FetchContent dependencies were pre-cloned to `D:\Project\Zmbor\deps` and passed as
  `-DFETCHCONTENT_SOURCE_DIR_{ELFIO,TOML11,FMT,LIBDWARF,RABBITIZER,RAYLIB,IMGUI,RLIMGUI,NLOHMANN_JSON}=...`
  (the default toml11 fetch pulls large submodules and stalls).
- Existing build directories:
  - `D:\Project\Zmbor\wt-integration\build` is configured without hardware tests, with IOP tests on.
  - `D:\Project\Zmbor\wt\build` is the pre-integration tree configured with `PS2X_HWTEST_GENERATED`.
- Without `PS2X_HWTEST_GENERATED` the build takes about 2 minutes. With it, about 12 minutes, because the 55
  generated test programs compile at /Od. /O2 made it 55 minutes (`PS2X_HWTEST_OPTIMIZE`).
- Debug builds (`build-debug`) work (CMP0141 /Z7).

## Tests

```
build\ps2xTest\ps2x_tests.exe                       # unit; run from the repo root (reads ps2xRecomp headers)
ctest --test-dir build\ps2xTest -LE hwtest -j 8     # research DB, GS cache, e2e
ctest --test-dir build\ps2xIOP -j 4
```

Hardware result tests (see `TESTING.md` level 5, `docs/hwtest/README.md`):

```
python tools\hwtest\hwtest.py prepare --tests D:\Project\Zmbor\thirdparty-data\ps2autotests\tests ^
  --recompiler build\ps2xRecomp\ps2_recomp.exe --out D:\Project\Zmbor\hwtest_work\gen
cmake --build build                                  # after configuring with PS2X_HWTEST_GENERATED
ctest --test-dir build\ps2xTest -L hwtest -j 8
python tools\hwtest\hwtest.py report --out D:\Project\Zmbor\hwtest_work\gen --json runN.json --md runN.md ^
  --baseline <previous runN.json>
```

- Rerun `prepare` after any recompiler change: the generated C++ is the recompiler's output.
- ps2autotests (ISC, commit 97469ffb) is cloned outside the repo at `D:\Project\Zmbor\thirdparty-data\ps2autotests`.
  Neither its ELFs nor the generated code are committed; the `.expected` files are never edited.
- Dashboards: `docs/hwtest/dashboard.json` (latest, run9) and `docs/hwtest/baseline-first-run.json` are in the
  repo. The run history `baseline0.json`, `run1..run9.json/.md` lives in `D:\Project\Zmbor\hwtest_work`.
- To diff one test, compare `hwtest_work/gen/<id>/run.stdout.txt` with the test's `.expected`.
- Mutation checking: revert one fix, rebuild `ps2x_tests`, and confirm its regression test fails. Every Phase 2/3
  fix was checked this way.

## Architecture notes (where the Phase 2/3 fixes live)

- Recompiler: `ps2xRecomp/src/lib/elf_parser.cpp` (function starts; the delay-slot filter),
  `control_flow_emitter.cpp` (branches; BC0 → `runtime->memory().dmacCpcond()`), `code_generator.cpp`.
- EE address map: `ps2xRuntime/include/runtime/ps2_address.h`. VU0 micro memory and VU1 data are a "special" range,
  not RDRAM.
- VU: `ps2xRuntime/src/lib/vu/`. The interpreter runs under `FE_TOWARDZERO`. EFU math (`ps2_vu1_lower.cpp`) runs
  to-nearest inside `EfuRoundingScope` and was fitted to 208 hardware vectors (`ps2xTest/src/ps2_efu_hw_vectors.inc`,
  generator `ps2xTest/tools/gen_efu_hw_vectors.py`). `vuAdd` drops operands shifted out by 25+ exponent steps.
  VU1 starts on VIF1 MSCAL and on CTC2 CMSAR1 (`PS2Runtime::vu1StartMicroProgram`).
- VIF: `ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp`.
  - `unpackVif` is shared by VIF0 and VIF1.
  - Commands split across transfers are kept in `m_vifPartialCommand` (VPS = 1).
  - I-bit stalls hold the following data. The DMA stays active only when the held data exceeds the FIFO (8/16 qwords).
- DMAC: `ps2xRuntime/src/lib/ps2_memory.cpp`. `walkSourceChain` (GIF/VIF/toSPR source chains) and
  `tryProcessScratchpadDma` (SPR normal, interleave, toSPR chain).
- Hardware-test executables reserve a 64 MB stack (`ps2xTest/e2e/CMakeLists.txt`), because /Od host frames reach about 0.7 MB.

Phase 4 additions:
- Delay-slot control transfers: `ControlFlowEmitter::emitTransferInDelaySlot` (model and evidence in its comment); the
  analyzer adds the entry points, `FunctionEmitter` emits the inner transfer as its own instruction.
- EE memory ownership: `PS2Runtime::runtimeMalloc/runtimeFree` (arena `kRuntimeArenaBase..kRuntimeArenaEnd`, EE kernel
  area; 0 on exhaustion, never a guest-heap fallback; an ELF overlapping the reserved area is refused, `describeRuntimeReservedOverlap`) for
  runtime-owned memory; `guestMalloc` only for HLE malloc. HLE pools in `Syscalls/Helpers/State.h` sit at
  0x0004F000-0x00080000. SetupHeap(-1) ends at `guestMainStackBase()` (recorded by SetupThread).
- COP0 PCCR/PCR0/PCR1: `PS2Runtime::read/writePerfControl/Counter`, counted from `EeScheduler::currentEeCycle()`.
- COP1 MUL.S: `ps2_ee_fmul_bits` in `ps2_runtime_macros.h` (VU multiplies are separate and unchanged).

## Completed milestones

- Phase 0: upstream PRs #304/#268/#310 integrated with extensions, plus the #258 invocation-stack fix
  (`docs/INTEGRATION_MATRIX.md`).
- Phase 0b community research: 15 repositories plus a 205-fork scan, gathered into a 185-record database in `research/`.
  - The database has a schema and a validator that runs in ctest.
  - 12 fixes were integrated, including consensus fixes from the SotC, KZ and DQ8 projects.
  - Reports and the license review are in `docs/community-research/`; queued ideas are in `REUSABLE_IMPROVEMENTS.md`.
- Phase 1:
  - End-to-end harness `ps2xTest/e2e`: a Python R5900 assembler that needs no PS2SDK, plus a GS frame check.
  - MMI PMULTH family fixed against real-PS2 vectors.
  - Debug build fixed.
- Phase 2: hardware result pipeline (`tools/hwtest/hwtest.py`), plus the FPU, kernel and VU0 fixes it exposed.
- Phase 3:
  - VU microcode execution: 6 of 7 VU tests exact.
  - VIF, SPR and DMAC: all 11 `dma/*` tests exact.
- Phase 4: branch in a delay slot, heap ownership, COP0 performance counters, MUL.S, VU0 vi24: 53/55.

## Remaining hardware failures (2, after Phase 5)

| test | lines | cause / lead |
|---|---|---|
| vu_lower_efu | 182/223 | EATAN/ESIN/EEXP last bits (unit vectors 167/208). Production is the Phase 3 model. A Phase 5 candidate (FMAC adder in the series + pi/4 = 0x3F490FDA, 183/223) was reverted at the merge gate because it was selected on the scored rows; it stays reproducible in `tools/experiments/efu_ab.ps1`. EATAN(0)-type rows read 0x33E60000 on hardware, a cancellation residual no candidate reproduces (register 2.5) |
| cpu_ee_fpu_sqrt | 59/66 | RSQRT.S: not SQRT.S-then-DIV.S (that composition predicts 29/34 distinct rows); a post-hoc reciprocal datapath fits 34/34 but is not adopted; probe P3 separates them (register 1.5). Also sqrt(-0) = +0 on hardware |

Fixed in Phase 4: cpu_ee_branchdelay, cpu_ee_fpu_branchdelay, memory_ee_default, cpu_ee_cop0_performance,
cpu_ee_fpu_muldiv, cpu_vu0_macro_transfer (see `CHANGELOG.md`, Phase 4).

## Next fundamental milestone (Phase 6, proposed)

Rule: nothing below is implemented on assumption. Each item first produces evidence (category A/B) or a falsifiable
model with a probe; production changes only when the evidence picks one model.

| # | item | needs | first deliverable | blocked on |
|---|---|---|---|---|
| 1 | Hardware evidence for P1–P4 | ps2sdk/ps2dev toolchain, a PS2 with ps2link or uLaunchELF, the owner's consent to run homebrew | build the four generated probes in ps2autotests, capture the `fs ft result` lines, `probe_lab.py ingest --source hardware --console <id>` | console + toolchain |
| 2 | Macro-mode MAC/STATUS flags (register 2.4, gap) | a probe that runs COP2 VADD/VMUL in macro mode, then CFC2 vi16/vi17 (MAC/STATUS), on boundary operands | probe generator + expected-flag candidates (same as micro / not updated / delayed) | item 1's setup |
| 3 | EE ADD guard bits (register 1.2) | P1 results (gaps 1–24, opposite signs) | pick M1 or M2(g); change `ps2_fadd` only then; keep the Z3 report as the design record | item 1 |
| 4 | RSQRT.S datapath (register 1.5) | P3 results, more operand pairs from the same generator | decide composed vs reciprocal; a non-post-hoc validation set first | item 1 |
| 5 | EFU remainder (register 2.5, H-EFU-1, EATAN(0) residual) | a Python EFU model independent of the interpreter, then an EFU probe | reproduce the 0x33E60000 residual from a stated alignment rule, or record its failure | model work; probe needs item 1 |
| 6 | Native runtime profile | a representative workload: a user-supplied game ELF (never committed) or a homebrew 3D demo; Tracy or Windows ETW | time split per subsystem (EE code, VU interpreter, GS, VIF/DMA) before any further optimisation | workload |

Also open from the Phase 5 review: when a MADD/MSUB product overflows but the final sum does not, micro mode now
reports O only as sticky (from the product), not as a current flag; hardware behaviour is unmeasured (item 2).
ctest `e2e` and `hwtest` have never run on Linux.
