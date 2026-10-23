# Zmbor — handoff (2026-10-24)

Official repository: **https://github.com/m7kor/Zmbor**. Zmbor is the continuation of the "PS2Recomp Ultimate" work,
built on the upstream project `ran-j/PS2Recomp@eb8688b` plus upstream #384. Re-measure before quoting any number.

## State

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

## Remaining hardware failures (8)

| test | lines | cause / lead |
|---|---|---|
| vu_lower_efu | 182/223 | 41 rows: EATAN/ESIN/EEXP last-bit differences; exact series term rounding unknown (model matches 167/208 vectors) |
| cpu_ee_fpu_muldiv | 342/360 | MUL.S gives x-1ulp for 1.0*x with all-ones mantissa; needs a bit-level EE multiplier model (PCSX2's is GPL-3, do not copy) |
| cpu_ee_fpu_sqrt | 59/66 | RSQRT.S 1 ulp low; same bit-level divider issue |
| cpu_ee_branchdelay | 12/13 | branch in a branch delay slot: hardware runs one instruction at the second target; recompiler does not model it |
| cpu_ee_fpu_branchdelay | 2/4 | same, with BC1x |
| cpu_ee_cop0_performance | 6/13 | PCCR write mask, PCR0/PCR1 event counting |
| memory_ee_default | 12/13 | `malloc(30 MB)` fails: EndOfHeap 0x01F00000; runtime `guestMalloc` shares the region with guest sbrk |
| cpu_vu0_macro_transfer | 35/36 | `cfc2 vi24` reads 0x0c0c after `ctc2 -1` on hardware; unexplained |

## Phase 4 priorities (suggested)

1. Branch in a delay slot (two tests; a recompiler change in the control-flow emitter, testable by e2e).
2. Heap limit: separate the runtime's `guestMalloc` arena from the guest heap/sbrk, then raise EndOfHeap.
3. COP0 performance counters (PCCR mask, PCR0/1 events).
4. Bit-level FPU MUL.S / RSQRT.S models built from the hardware vectors.
5. EFU series last bits (time-box; diminishing returns).
6. Integrate the queued community items in `docs/community-research/REUSABLE_IMPROVEMENTS.md` that the hardware tests
   do not cover, then validate on real game ELFs supplied by the user. Never commit game data or BIOS.
