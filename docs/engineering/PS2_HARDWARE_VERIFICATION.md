# PS2 hardware verification

## Evidence levels (tools/hwcoverage)

| level | meaning | source |
|---|---|---|
| `contract_specified` | a verification contract exists | `docs/engineering/PS2_HARDWARE_REQUIREMENTS.json` |
| `test_exists` | the component cites tests that the validator found | `docs/status/components.json` |
| `test_executed` | a cited named test ran in the recorded run | `docs/status/test-results.json` |
| `test_passed` | every cited named test passed | same |
| `vectors_passed` | tests whose expected values are real-PS2 results passed | `hwvec` references |
| `hardware_verified` | every cited ps2autotests program matches real-PS2 output | `docs/hwtest/dashboard.json` |
| `timing_verified` | timing checked against hardware | none yet |

`python tools/hwcoverage/hwcoverage.py record --tests-exe build/ps2xTest/ps2x_tests.exe` after a build;
`python tools/hwcoverage/hwcoverage.py report` to regenerate the report and the gap matrix (CI checks they are fresh).
A component's status is never raised by these levels automatically.

## Hardware probe workflow
1. Write the probe as ps2sdk homebrew; record source revision, compiler flags and binary hash.
2. Freeze the predictions of every candidate model (hash them) before running.
3. Run on a physical PS2; record console model and the raw output.
4. Run the same probe through Zmbor; compare exactly.
5. Adopt a model only if it was fixed before the run and matches every row, including held-out rows.
6. Register the observation (`docs/architecture/evidence-register.md`) and cite it from the component.

No physical PS2 or ps2sdk toolchain is available in this environment, so steps 3-6 are blocked; probes are written
and their predictions frozen so that they can run when hardware is available.

## Probes

| probe | component(s) | state |
|---|---|---|
| P1-P5 | EE ADD guard bits, EE MUL, RSQRT.S datapath, VU MUL, VU0 macro flags | written (`tools/hwprobes`), not run |
| GS dither | `gs.dither`, `gs.alpha` (COLCLAMP) | specified: flat-colour quads over a DIMX with entries -4, -1, +1, +3 in PSMCT16 and PSMCT32, with COLCLAMP 0 and 1, read back by local-to-host transfer; settles D-GS-1/2 |
| MFIFO | `dma.mfifo` | specified: GIF and VIF1 drains, stall point (TADR, MADR, QWC while stalled), MEIS timing, fromSPR into the ring with MFD=0 and with MADR outside the ring; settles D-DMA-1/2 |
| Stall control | `dma.stall`, `dma.irq` (SIS) | specified: fromSPR source (STS=2) in normal and interleave mode feeding a GIF drain (STD=2) through REFS tags and in normal mode, and a VIF1 drain (STD=1); D_STADR read after each source transfer; drain MADR, QWC, TADR and CHCR.STR, D_STAT.SIS read while the drain waits, by polling and from a cause-13 handler; drain ending exactly at D_STADR; settles D-DMA-4 (granularity, normal-mode drain) |
| EFU | `vu.efu` | to write: the 41 mismatching inputs and their ±1 ulp neighbours, per EFU op |
