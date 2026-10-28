# Autonomous execution state

Machine-readable copy: `autonomous-execution-state.json`. On resume, compare both with `git log` and
`gh pr view 7 --repo m7kor/Zmbor` before continuing; the commit fields describe the branch when the file was last
committed.

## Where things are

- PR: https://github.com/m7kor/Zmbor/pull/7 (Draft), branch `ultimate/ps2-hardware-system-engineering`, base `main`
  `ec4a32d`. Push with the explicit URL `https://github.com/m7kor/Zmbor.git` (`origin` in the worktrees is upstream).
- One PR only. No merge, no force-push, no history rewrite.

## Verified baseline (batch 11 gate, `0765383`, clean tree)

ps2autotests 53/55 programs and 8202/8250 lines (`.expected` unmodified), 0 crashes, timeouts or regressions;
`ps2x_tests` 598/598 (batch 10: 596/596 under ASan, 0 reports); ctest 54/54; e2e 4/4; IOP 4/4. ps2autotests IOP programs
(separate measurement): 23/23 exact, 1306/1306 lines. Hardware inventory (174): 18 / 52 / 63 / 20 / 17 / 3 / 1.

## Batches

| batch | commits | inventory change | evidence |
|---|---|---|---|
| 1 | `5fab12b`..`0e47a2b` | `gs.dither`, `dma.mfifo`: MISSING → IMPLEMENTED_UNVERIFIED | 4/4 GS and 8/8 DMA mutants killed; 30,977 ASan fuzz runs |
| 2 | `78b17e6`..`c64f357` | `dma.stall`: MISSING → IMPLEMENTED_UNVERIFIED | 5 unit tests, 10/10 mutants killed, 26,801 ASan fuzz runs; contracts for all 174 components with a checked dependency DAG |
| 11 | `0765383`..`0e345dc` | `gs.aa` MISSING → IMPLEMENTED_UNVERIFIED | GS PRIM.AA1 for lines and triangles (D-GS-1); 11/11 mutants |
| 10 | `a915237`..`d82a40a` | `ipu.cmd` MISSING → IMPLEMENTED_UNVERIFIED | IPU BDEC (D-IPU-3) and IDEC (D-IPU-4); bit-exact on 6 ffmpeg MPEG-1/2 fixtures; pause/resume, truncation, RST, 576 corrupted slices; 30/30 non-equivalent mutants; semantics ledger `PS2_IPU_SEMANTICS.md` |
| 9 | `b2f7ce1`..`6f93475` | none (`ipu.cmd` stays MISSING) | IPU VDEC (MPEG-2 VLC tables); 4/4 mutants |
| 8 | `ff3a138`..`af37990` | none (`ipu.cmd` stays MISSING) | IPU PACK (D-IPU-2); 3/3 mutants |
| 7 | `0828791`..`d15ce5a` | `dma.c3`, `ipu.dma` MISSING → IMPLEMENTED_UNVERIFIED | IPU CSC (D-IPU-1), output FIFO, fromIPU; 6/6 CSC mutants |
| 6 | `6ecc5f3`..`038536f` | `dma.c4` MISSING → IMPLEMENTED_UNVERIFIED | IPU core (registers, input FIFO, BCLR/FDEC/SETIQ/SETVQ/SETTH, RST, INTC 8) and toIPU; 6/6 mutants; `ipu.cmd`/`ipu.dma` stay MISSING |
| 5 | `a1f2ddc`..`46c4cf8` | `iop.irx`, `iop.mem` → VERIFIED_PARTIAL; `dma.c6` HLE_OR_STUB → IMPLEMENTED_UNVERIFIED | IOP evidence cited; SIF1 channel-6 model with stall drain, 5/5 mutants; IOP kernel clean under ASan |
| 4 | `3696d34`..`e2be153` | `iop.kernel`, `iop.intr`: HLE_OR_STUB → VERIFIED_PARTIAL | IOP kernel model (start thread, preemption, typed waits, message boxes, variable pools), VBLANK, intrman, strtok; 15/15 kernel mutants; IOP programs 8/23 → 23/23 |
| 3 | `2318aad`..`7181e0f` | `iop.cpu`: IMPLEMENTED_UNVERIFIED → VERIFIED_PARTIAL | IRX relocation, IOP printf and JALR fixes, each with a test that fails without it; IOP programs 0/23 → 8/23 (cpu/iop 7/7) |

## Blockers

- No physical PS2, ps2link or ps2sdk here: every probe in `PS2_HARDWARE_VERIFICATION.md` is unrun and no new
  hardware observation can be taken. Hardware evidence comes only from real-PS2 output that already exists
  (ps2autotests `.expected`, EE and IOP).
- SIF0 (EE channel 5 / IOP channel 9) has no DMA channel model: an unresolved architectural task, not settled by the
  HLE path, which is not a permanent substitute. Both ends (IOP sifman, EE reception) are HLE and no program drives
  channel 5. SIF1 (batch 5) and toIPU/fromIPU (batches 6-7) are modelled.
- The IPU's PS2-specific behaviour (timing, IPU_CMD after decoding, IDCT rounding, error recovery: B1-B11 in
  `PS2_IPU_SEMANTICS.md`) needs an IPU probe on a physical PS2.

## Known failures

`cpu_ee_fpu_sqrt` 59/66 (RSQRT.S) and `vu_lower_efu` 182/223 (EATAN/ESIN/EEXP last bits): unchanged, need hardware
probes.

## Owner decisions pending

- Merging PR #7.

## Owner decisions recorded

- `per.bios`, `per.ps1`, `per.dvdv` stay NOT_APPLICABLE for the native recompilation/runtime scope (2026-10-28).

## Next action

SPU2 in dependency order: a register model for both cores and the 2 MB sound RAM (`spu.cores`, `spu.ram`), with
16-bit register access, the TSA data port, IRQA and the IOP DMA channels 4 and 7. Then voices, ADSR, pitch, mixer,
reverb and interrupts. No ps2autotests program exercises the SPU2, so it can reach IMPLEMENTED_UNVERIFIED at most.
SIF0 stays an open architectural task.
