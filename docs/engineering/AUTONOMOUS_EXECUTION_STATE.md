# Autonomous execution state

Machine-readable copy: `autonomous-execution-state.json`. On resume, compare both with `git log` and
`gh pr view 7 --repo m7kor/Zmbor` before continuing; the commit fields describe the branch when the file was last
committed.

## Where things are

- PR: https://github.com/m7kor/Zmbor/pull/7 (Draft), branch `ultimate/ps2-hardware-system-engineering`, base `main`
  `ec4a32d`. Push with the explicit URL `https://github.com/m7kor/Zmbor.git` (`origin` in the worktrees is upstream).
- One PR only. No merge, no force-push, no history rewrite.

## Verified baseline (batch 5 gate, `46c4cf8`, clean tree)

ps2autotests 53/55 programs and 8202/8250 lines (`.expected` unmodified), 0 crashes, timeouts or regressions;
`ps2x_tests` 567/567; ctest 54/54; e2e 4/4; IOP 4/4. ps2autotests IOP programs (separate measurement): 23/23 exact,
1306/1306 lines. Hardware inventory (174): 18 / 52 / 58 / 20 / 22 / 3 / 1.

## Batches

| batch | commits | inventory change | evidence |
|---|---|---|---|
| 1 | `5fab12b`..`0e47a2b` | `gs.dither`, `dma.mfifo`: MISSING → IMPLEMENTED_UNVERIFIED | 4/4 GS and 8/8 DMA mutants killed; 30,977 ASan fuzz runs |
| 2 | `78b17e6`..`c64f357` | `dma.stall`: MISSING → IMPLEMENTED_UNVERIFIED | 5 unit tests, 10/10 mutants killed, 26,801 ASan fuzz runs; contracts for all 174 components with a checked dependency DAG |
| 5 | `a1f2ddc`..`46c4cf8` | `iop.irx`, `iop.mem` → VERIFIED_PARTIAL; `dma.c6` HLE_OR_STUB → IMPLEMENTED_UNVERIFIED | IOP evidence cited; SIF1 channel-6 model with stall drain, 5/5 mutants; IOP kernel clean under ASan |
| 4 | `3696d34`..`e2be153` | `iop.kernel`, `iop.intr`: HLE_OR_STUB → VERIFIED_PARTIAL | IOP kernel model (start thread, preemption, typed waits, message boxes, variable pools), VBLANK, intrman, strtok; 15/15 kernel mutants; IOP programs 8/23 → 23/23 |
| 3 | `2318aad`..`7181e0f` | `iop.cpu`: IMPLEMENTED_UNVERIFIED → VERIFIED_PARTIAL | IRX relocation, IOP printf and JALR fixes, each with a test that fails without it; IOP programs 0/23 → 8/23 (cpu/iop 7/7) |

## Blockers

- No physical PS2, ps2link or ps2sdk here: every probe in `PS2_HARDWARE_VERIFICATION.md` is unrun and no new
  hardware observation can be taken. Hardware evidence comes only from real-PS2 output that already exists
  (ps2autotests `.expected`, EE and IOP).
- SIF0, SIF1 and fromIPU have no DMA channel model in `PS2Memory` (HLE only). Stall control with those channels is
  blocked on the models.

## Known failures

`cpu_ee_fpu_sqrt` 59/66 (RSQRT.S) and `vu_lower_efu` 182/223 (EATAN/ESIN/EEXP last bits): unchanged, need hardware
probes.

## Owner decisions pending

- Merging PR #7.

## Owner decisions recorded

- `per.bios`, `per.ps1`, `per.dvdv` stay NOT_APPLICABLE for the native recompilation/runtime scope (2026-10-28).

## Next action

SIF0: EE channel 5 destination chain fed by IOP channel 9 packets (IOP side in ps2xIOP sifman) with the STS=1 stall
source; then IPU, GS (`gs.aa`) and SPU2 in dependency order. `cpu/iop/lsudelay` still loops after its END marker.
