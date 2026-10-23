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
| current (Phase 3) | 47 / 55 | 8172 / 8250 (99.1%) |

Fully matching areas: `kernel/ee` 10/10 (threads, interrupts), `dma` 11/11 (VIF, SPR, DMAC), `cpu/ee_simd` 6/6,
`gs` 2/2, `vu` 6 of 7, 6 of 9 `cpu/ee_fpu`, 5 of 6 `cpu/ee`. No crash or timeout. No test that passed in an earlier
run fails in a later one.

## Remaining failures, by cause

| area | tests | what differs | status |
|---|---|---|---|
| FPU multiply/divide rounding | fpu_muldiv, fpu_sqrt | MUL.S gives x-1ulp for 1.0*x with an all-ones mantissa; RSQRT.S 1ulp low | needs a bit-level model of the EE multiplier/divider built from the vectors (PCSX2's model is GPL-3, not reused) |
| VU EFU series | vu/lower/efu (182/223 lines) | EATAN/ESIN/EEXP differ in the last bit on 41 rows | the EFU model (`ps2_vu1_lower.cpp`) matches 167 of 208 hardware vectors; the exact term rounding of the series is not known |
| COP0 performance counters | cop0_performance | PCCR write mask and PCR0/PCR1 counting | |
| Branch in a delay slot | branchdelay, fpu_branchdelay | the hardware follows the second branch's target for one instruction | not modelled by the recompiler |
| Heap limit | memory/ee/default | `malloc(30 MB)` fails: EndOfHeap stops at 0x01F00000 | the runtime's own guestMalloc shares that region with the guest's sbrk; raising the limit needs the two separated first |
| VU0 reserved register | vu0_macro/transfer | `cfc2 vi24` reads 0x0c0c on hardware after `ctc2 -1` | no explanation found; left as is |
