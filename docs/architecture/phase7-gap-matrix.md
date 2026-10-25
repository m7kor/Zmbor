# Phase 7 — gap matrix

Status words: **verified** = checked against hardware evidence (ps2autotests `.expected`) or proved; **partial** =
some behaviour verified, the rest not; **unsupported** = not implemented (HLE stub or absent); **untested** =
implemented with no test that could fail on wrong behaviour. Counts come from the Phase 7 baseline
(`p7_baseline.manifest.json`, commit `d4310f1`) and a source inventory of the same commit.

## Baseline (main `d4310f1`, MSVC 14.44 Release, local)

| check | result |
|---|---|
| ps2autotests hardware matrix (real-PS2 `.expected`, unmodified) | 53/55 programs, 8202/8250 lines, 0 crashes, 0 timeouts, 0 regressions vs Phase 6 |
| open hardware divergences | `cpu_ee_fpu_sqrt` 59/66 (RSQRT.S rows), `vu_lower_efu` 182/223 (EFU) |
| `ps2x_tests` | 545/545 |
| ctest (non-hwtest) | 53/53 |
| e2e | 4/4 |
| `ps2xIOP` | 4/4 |

Identical to the Phase 6 release gate (`p6_release.json`).

## Subsystems

| subsystem | status | evidence / gap | blocker for "verified" |
|---|---|---|---|
| EE integer ALU, MMI | verified (hardware matrix) + proved (Phase 7: overflow predicates, PLZCW, DIV guards) | ps2autotests cpu/ee/*, ee_simd/*; `tools/semantics/z3_int_semantics.py` | — |
| EE COP1 (FPU) | partial | all rows pass except RSQRT.S | `tools/semantics/ee_rsqrt_models.py` (re-run 2026-10-25): the pre-registered composition SQRT∘DIV fits at most 30/34 RSQRT rows. A datapath `a * R(1/R(sqrt b))` fits 34/34, but it was chosen after seeing those 34 rows and the same datapath scores 23/31 on DIV.S, so it is a post-hoc fit with no held-out confirmation and is **not** adopted. Needs new RSQRT.S hardware rows (no PS2). |
| VU0/VU1 FMAC, pipelines | verified for the tested programs | vu_upper_*, lockstep reference mode (Phase 6) | — |
| VU EFU | partial (182/223) | EFU polynomial results differ in low bits | EFU coefficient/rounding data from hardware; none available. |
| VU0 macro flags | untested on hardware | probe P5 prepared (Phase 6), not run | Physical PS2 + ps2sdk. |
| VIF0/VIF1 | partial | unit tests; fuzzed in Phase 7 (`ps2x_fuzz_vif0/1`) | no hardware traces for stall/MARK/interrupt timing |
| DMAC | partial | chain modes unit-tested; fuzzed through registers (`ps2x_fuzz_dma`) | timing/arbitration is not modelled cycle-accurately |
| GIF / GS | partial | CPU rasteriser ~3800 lines, GS unit tests, e2e images; fuzzed (`ps2x_fuzz_gif`) | no GS reference dumps from hardware; parallel-gs spike needs Vulkan harness |
| IOP / SIF | partial | `ps2xIOP` 4/4, SIF RPC/DMA unit tests | HLE modules; no IOP instruction-level execution of real IRX |
| SPU2 | unsupported | audio is HLE (`Kernel/Stubs/Audio.cpp`: 90 TODO markers, `sceSd*`, `sceSSyn_*` named stubs) | no SPU2 emulation; would need hardware audio captures to validate |
| Kernel / syscalls | partial | kernel unit tests; 60 stub markers in 15 files | stubs return fixed values |
| Recompiler (ELF, CFG, codegen) | partial | e2e, codegen tests; ELF/decoder/emitter fuzzed (Phase 7) | indirect-call resume (JALR) kept as Phase 6 decided |
| Game compatibility | stage 0–4 only for in-repo programs | `tools/compat` | no commercial program run; stage 5–9 never credited |

## Known title-specific code

`game_overrides.cpp` is a generic descriptor registry (data, no per-title logic in the runtime); `games_database.cpp`
holds metadata; one comment in `MPEG.cpp` names a title as the origin of a test case. No output hardcoding was found.
