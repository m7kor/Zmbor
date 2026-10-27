# Hardware result tests: state

Source: the 55 prebuilt ELFs of [ps2autotests](https://github.com/unknownbrackets/ps2autotests) (ISC, commit
97469ffb), whose `.expected` files were printed by a real PS2. How to run: `TESTING.md`, level 5. The `.expected`
files are never edited; a test passes only when the guest's own output matches them exactly.

- `dashboard.json` — latest run (machine-readable: summary, per subsystem, per test, regressions).
- `baseline-first-run.json` — the first run of the pipeline, before any fix in Phase 2.

| run | exact tests | matching lines |
|---|---|---|
| first run | 6 / 54 (1 ELF did not recompile) | 5346 / 8240 (65%) |
| end of Phase 2 | 32 / 55 | 6627 / 8250 (80%) |
| end of Phase 3 | 47 / 55 | 8172 / 8250 (99.1%) |
| end of Phase 4 | 53 / 55 | 8202 / 8250 (99.4%) |
| current (Phase 7, gate on `c7e5960` = main `a13b47a`) | 53 / 55 | 8202 / 8250 (99.4%) |

`dashboard.json` is that Phase 7 gate run; `docs/status/components.json` records its SHA-256, so replacing it
requires re-measuring the status inventory (`tools/status/generate_dashboard.py measure`).

Fully matching areas: `kernel/ee` 10/10, `dma` 11/11, `cpu/ee` 6/6, `cpu/ee_cop0` 1/1, `cpu/ee_simd` 6/6,
`cpu/vu0_macro` 2/2, `memory/ee` 1/1, `gs` 2/2, `vu` 6 of 7, `cpu/ee_fpu` 8 of 9. No crash or timeout. No test that passed in an earlier
run fails in a later one.

## Remaining failures, by cause

| area | tests | what differs | status |
|---|---|---|---|
| RSQRT.S rounding | fpu_sqrt (59/66 lines) | 1 ulp on 7 rows (e.g. `rsqrt 3, 3` gives 3fddb3d7, we give 3fddb3d8) | no simple model fits all 39 RSQRT rows: root to nearest with a truncated division fits 38, and DIV.S rounds differently (1/0x3FFFFFFF rounds up at 0.50 ulp, an RSQRT quotient at 0.52 ulp does not) |
| VU EFU series | vu/lower/efu (182/223 lines) | EATAN/ESIN/EEXP differ in the last bit on 41 rows | the EFU model (`ps2_vu1_lower.cpp`) matches 167 of 208 hardware vectors; the exact term rounding of the series is not known |

Fixed in Phase 4 (see `CHANGELOG.md`): branch in a delay slot, heap ownership (memory/ee/default), COP0 performance
counters, MUL.S truncation and non-commutativity, VU0 vi24.

## IOP programs (separate measurement)

ps2autotests also ships 23 prebuilt IOP modules (`tests/cpu/iop`, `tests/kernel/iop`, `.irx`) with real-PS2 `.expected`
output. They run on the ps2xIOP emulator through `tools/hwtest/iop_hwtest.py` (runner `ps2_iop_autotest_runner`, same
BEGIN/END extraction and comparison as the EE programs) and are reported in `iop-dashboard.json` / `IOP_DASHBOARD.md`.
They are not added to the 55 EE programs above.

| run | exact programs | matching lines |
|---|---|---|
| first run (printf formatting in place, before the IRX relocation fix) | 0 / 23 (no output) | 0 / 1306 |
| current | 7 / 23 | 1167 / 1306 |

Exact: `cpu/iop` alu, branch, hilodelay, lsu, lsudelay, muldiv; `kernel/iop/libc` sprintf. `cpu/iop/branchdelay`
differs on 1 of 11 lines; the 15 other `kernel/iop` programs (event flags, message boxes, threads, vpool, strtok)
differ where the HLE IOP kernel's results differ from the real kernel's.