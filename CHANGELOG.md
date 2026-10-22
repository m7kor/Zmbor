# Changelog

Changes in this fork relative to `ran-j/PS2Recomp@eb8688b`. Every entry was built with MSVC 14.44 + Ninja
(Release, `PS2X_BUILD_STUDIO=OFF`, `PS2X_ENABLE_FFMPEG=OFF`) on Windows 11 x64 and verified with
`ps2x_tests` and the `ps2xIOP` ctest suite. Generated game code must be regenerated to pick up recompiler fixes.
No commercial game was run to validate these changes; game-level reports are cited from the linked issues.

## Phase 0 — recompiler and runtime correctness

Baseline before changes: `ps2x_tests` 445/445.
After: `ps2x_tests` 454/454, `ps2xIOP` ctest 4/4.

### Recompiler
- Delay slots are never silently dropped (#370). Integrates PR #304 (decode the word after a function ending in a
  branch) and extends it: function slices keep their trailing delay slot, and the emitter decodes the real word
  from the ELF section, with a warning, if one is still missing instead of substituting a NOP.
- `vcallmsr` starts the microprogram from CMSAR0 instead of reading out of bounds at `vi[27]` (PR #268, #262).
- VU0 macro `LQI/LQD/SQI/SQD/ILWR/ISWR` decode their operand fields correctly and access VU0 data memory instead of
  EE RAM `0x0000..0x3FF0` (PR #310, #309). This fork also keeps `vi0`/`vf0` hardwired for these instructions.
- `CodeGenerator` owns its section list (it held a reference that tests bound to a temporary).

### Runtime
- EE invocation stacks (interrupt handlers, callbacks, syscall overrides, exit handlers) are allocated below the
  ELF image instead of on top of the main thread's stack, and a thread's stacks are reused after it goes Dormant
  (#258). The top-of-RAM region remains only as a logged fallback.

See `docs/INTEGRATION_MATRIX.md` for the evidence behind each decision, including the proposals not taken (#260).

## Phase 0b — community engineering integration (2026-10-23)

After: `ps2x_tests` 466/466, `ps2xIOP` ctest 4/4, `research_database_valid` ctest passing.
Every fix below was checked against PCSX2 source and has a regression test that fails when the fix is reverted.

- Signed branches test bit 63; SQRT.S reads ft (from the SotC fork, also found by DQ8, Killzone, Def Jam, BT3).
- VF00 = (0,0,0,1) in every context; LQ/SQ/LQC2/SQC2 align to 16 bytes (SotC, Killzone, God of War).
- DMA source chains are no longer cut at 4096 tags (SotC).
- CVT.W.S truncates and saturates; VU0 DIV/SQRT/RSQRT follow the FDIV-unit rules with I/D flags; PEXEW lanes.
- `SetupThread(-1)` records the stack base (#259); `D_ENABLEW` mirrors to `D_ENABLER` (PR #284).
- Community research database: 185 records from 4 forensic passes over 15 listed projects plus a 205-fork scan,
  with a schema, a validator run by ctest, and generated reports in `docs/community-research/`.
