# Autonomous execution state

Machine-readable copy: `autonomous-execution-state.json`. On resume, compare both with `git log` and
`gh pr view 7 --repo m7kor/Zmbor` before continuing; the commit fields describe the branch when the file was last
committed.

## Where things are

- PR: https://github.com/m7kor/Zmbor/pull/7 (Draft), branch `ultimate/ps2-hardware-system-engineering`, base `main`
  `ec4a32d`. Push with the explicit URL `https://github.com/m7kor/Zmbor.git` (`origin` in the worktrees is upstream).
- One PR only. No merge, no force-push, no history rewrite.

## Verified baseline (batch 2 gate, `c64f357`)

ps2autotests 53/55 programs and 8202/8250 lines (`.expected` unmodified), 0 crashes, timeouts or regressions;
`ps2x_tests` 563/563; ctest 54/54; e2e 4/4; IOP 4/4. The gate flagged the tree as not clean only because of untracked
new files that no build target includes. Hardware inventory (174): 18 / 47 / 60 / 23 / 22 / 3 / 1.

## Batches

| batch | commits | inventory change | evidence |
|---|---|---|---|
| 1 | `5fab12b`..`0e47a2b` | `gs.dither`, `dma.mfifo`: MISSING → IMPLEMENTED_UNVERIFIED | 4/4 GS and 8/8 DMA mutants killed; 30,977 ASan fuzz runs |
| 2 | `78b17e6`..`c64f357` | `dma.stall`: MISSING → IMPLEMENTED_UNVERIFIED | 5 unit tests, 10/10 mutants killed, 26,801 ASan fuzz runs; contracts for all 174 components with a checked dependency DAG |
| 3 (in progress) | not committed | none yet | IOP printf/sprintf formatting and a runner for the real-PS2 IOP programs |

## Blockers

- No physical PS2, ps2link or ps2sdk here: every probe in `PS2_HARDWARE_VERIFICATION.md` is unrun, so no component
  can reach hardware-verified status in this environment.
- SIF0, SIF1 and fromIPU have no DMA channel model in `PS2Memory` (HLE only). Stall control with those channels is
  blocked on the models.

## Known failures

`cpu_ee_fpu_sqrt` 59/66 (RSQRT.S) and `vu_lower_efu` 182/223 (EATAN/ESIN/EEXP last bits): unchanged, need hardware
probes.

## Owner decisions pending

- Three NOT_APPLICABLE scope proposals in `docs/status/components.json`.
- Merging PR #7.

## Next action

Batch 3: IOP `printf` and `sprintf`/`vsprintf` copy the format string without formatting its arguments, so the
ps2autotests IOP programs (`tests/cpu/iop`, `tests/kernel/iop`, prebuilt `.irx` with real-PS2 `.expected`) cannot match.
Wire `iop_format.cpp` into stdio and sysclib, add `IopHost::consoleWrite`, build `ps2_iop_autotest_runner`, run
`tools/hwtest/iop_hwtest.py`, and cite only matching programs (`iop.cpu` first).
