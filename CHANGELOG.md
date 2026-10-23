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
The ISA/VU fixes were checked against PCSX2 source; #259 against Play!'s HLE kernel; PR #284 cites PCSX2/ps2tek.
All have regression tests; for the community-sourced fixes each test was also confirmed to fail with its fix reverted
(not done for #259 and #284).

- Signed branches test bit 63; SQRT.S reads ft (from the SotC fork, also found by DQ8, Killzone, Def Jam, BT3).
- VF00 = (0,0,0,1) in every context; LQ/SQ/LQC2/SQC2 align to 16 bytes (SotC, Killzone, God of War).
- DMA source chains are no longer cut at 4096 tags (SotC).
- CVT.W.S truncates and saturates; VU0 DIV/SQRT/RSQRT follow the FDIV-unit rules with I/D flags; PEXEW lanes.
- `SetupThread(-1)` records the stack base (#259); `D_ENABLEW` mirrors to `D_ENABLER` (PR #284).
- Community research database: 185 records from 4 forensic passes over 15 listed projects plus a 205-fork scan,
  with a schema, a validator run by ctest, and generated reports in `docs/community-research/`.

## Phase 0c — VU1 and VIF batch (2026-10-23)

After: `ps2x_tests` 471/471, `ps2xIOP` ctest 4/4. Each fix was checked against PCSX2 source, and every new or
changed test was confirmed to fail with its fix reverted.

- VU1 EFU sub-opcodes decode at the numbers PCSX2's lower-op tables give (ERSQRT 0x79, ESIN 0x7C, EATAN 0x7D,
  EEXP 0x7E; 0x77/0x7F reserved), with matching latencies (from the DQ8 fork).
- VU1 EATAN used a mistyped series constant (-0.13085337519646 for -0.139085337519646); found while checking the above.
- VU1 JR/JALR read the current VI value; the delayed-VI rule applies only to conditional branches (DQ8 fork).
- A VIFcode with the I bit raises INTC VIF0/VIF1 (DQ8 fork). The accompanying VIF stall is not modelled yet.
- A GIF IMAGE left open by a DIRECT continues only in the next DIRECT payload, never in VIFcodes; a DIRECT cut at
  the end of the DMA data continues in the next transfer (SotC fork, also DQ8, Killzone, SRWZ).
- Two upstream tests encoded the old behaviour (JR's VI timing; raw pixels after a DIRECT) and were replaced with
  the ported community tests.

## Phase 0d — VIF stall (2026-10-23)

After: `ps2x_tests` 480/480, `ps2xIOP` ctest 4/4. Checked against PCSX2 (`Vif_Transfer.cpp`, `Vif0_Dma.cpp`,
`Vif1_Dma.cpp`, `Vif.cpp`); each rule's test fails with that rule reverted.

- An I-bit VIFcode now stalls VIF0/VIF1 (STAT.VIS) after it finishes, unless it is MARK or ERR.MII is set.
  FBRST.STP/FBK stall too (VSS/VFS). Data arriving during a stall is kept and runs in order on FBRST.STC;
  FBRST.RST drops it. A VIF DMA whose data is held stays running (CHCR.STR, no D_STAT/DMAC completion) until STC.
  Based on upstream PR #295, with PCSX2's MARK exception and the VIF0 side added.
- VIFn register reads (STAT, ERR, MARK, CODE, ITOPS, ...) now return the VIF state instead of the raw I/O map, so
  guests see INT/VIS/MRK/DBF; VIF0_FBRST and VIFn_ERR writes take effect; debug-panel ERR offsets fixed.
- Not modelled: stalls for VU1/PATH waits (FLUSH*, MSCAL while busy), FBK cancelling an in-flight DMA.

## Phase 1 — real execution and the God of War port (2026-10-23)

After: `ps2x_tests` 486/486, end-to-end 2/2, `ps2xIOP` ctest 4/4, research database valid.

- First ELF-to-native tests (`ctest -L e2e`): guest programs assembled from source, recompiled, compiled with the
  runtime, run, and checked. `basic` prints `E2E sum=5050 fact=3628800 b63=1 lq=1` from guest code; `gs_sprite`
  draws through GIF DMA (PATH3) and a VIF1 DIRECT (PATH2), and the presented 640x448 frame is checked pixel by
  pixel. Runtime options added for this: `PS2X_HIDDEN_WINDOW=1`, `PS2X_DUMP_FRAME=<file.ppm>`.
- MMI HI/LO family: exact PMULTH/PMADDH/PMSUBH/PHMADH/PHMSBH and 128-bit PMFHI/PMFLO/PMTHI/PMTLO/PMFHL/PMTHL
  (PMULTH from Waffle565/God-Of-War-Native-Port; the rest found by our audit), checked against PCSX2 and 160
  real-PS2 rows from ps2autotests.
- Waffle565/God-Of-War-Native-Port analysed: 27 records; IOP/VU1 AOT compilers and the GL backend recorded as
  architecture references, not integrated.
- RSQRT.S sign with ft = 0 confirmed against real-PS2 output (sign of fs; PCSX2 differs). No change needed.
- `BENCHMARKS.md` (first measured baselines) and `TESTING.md` added.

## Phase 2 — hardware result pipeline (2026-10-23)

After: `ps2x_tests` 502/502, end-to-end 2/2, research database valid; hardware result tests (55 ps2autotests
ELFs, outputs captured on a real PS2): first run 6/54 exact matches (5346/8240 lines), now 25/54 (6360/8240 lines),
no regressions between runs. Every fix below cites the `.expected` lines it was derived from; none was edited.

- `tools/hwtest/hwtest.py` + `ctest -L hwtest`: recompile each test ELF at build time, run it natively, compare the
  guest's text with the hardware output, and aggregate a dashboard (PASS / MISMATCH / CRASH / TIMEOUT / NO_OUTPUT /
  NOT_BUILT / RECOMPILE_FAIL, with regressions against a baseline). `PS2X_HWTEST_OPTIMIZE` (default OFF) compiles
  the generated code unoptimized: seconds instead of minutes per test.
- Packet-level IOP side of SIFCMD/SIFRPC (INIT_CMD -> SET_SREG, BIND/CALL/RDATA -> RPC_END, one packet per SIF0
  interrupt) and the ROM FILEIO tty write, so recompiled ps2sdk libraries (stripped ELFs, no named stubs) reach
  the IOP; `PS2X_TTY_LOG=<file>` collects the guest tty alone. Protocol from ps2sdk's EE and IOP sifcmd/sifrpc.
- Kernel: syscall 4 `Exit` ends the run; the run also ends once every thread is Dormant (ps2sdk `_exit` uses
  ExitDeleteThread); thread calls return the thread id or -1 and ReferThreadStatus the status (EE kernel, not IOP
  KE_* codes); SleepThread returns the thread id; Dormant threads return to their initial priority;
  AddIntc/DmacHandler honour `next` (front / before a handler / back) and a negative handler result ends the chain.
- EE: MOVZ/MOVN move only the low doubleword; BGEZALL/BLTZALL link when not taken; SA counts bytes
  (MTSA/MTSAB/MTSAH/MFSA, QFSRV); PSLLH/PSRLH/PSRAH use sa & 15; PSLLVW/PSRLVW/PSRAVW operand order and 64-bit
  sign extension; PABSH/PABSW read rt and saturate; PADDUH/PSUBUH saturate; PEXCH/PEXCW/PEXEH/PREVH/PROT3W/PINTH/
  PINTEH lanes; PDIVW/PDIVUW/PDIVBW write LO/HI only, divide PDIVBW by rt.h0, and no longer trap on INT_MIN/-1
  (this crashed the host); CVT.S.W rounds toward zero; CFC1/CTC1 follow FCR0 = 0x2E30, FCR16-31 = FCR31 and the
  FCR31 write mask, FCR31 resets to 0x01000001; VU0 macro VI ops discard writes to vi0. Checked by 268 + 45 new
  real-PS2 vectors (`ps2xTest/tools/gen_simd_hw_vectors.py`).
- DMA: normal-mode GIF/VIF transfers leave MADR past the data; scratchpad SADR keeps bits 0x3FF0 and SPR MADR
  has no bit 31.
- Recompiler: a race in single-file output could abort with "combined output completion queue is missing index".
- `gs_sprite` now compares the whole frame (286,720 pixels, tolerance 0) and prints its SHA-256.
- Tests confirmed to fail with their fix reverted: MOVN, PEXCH, all-dormant stop, handler cancel, MADR, FILEIO bind.

### Phase 2, second batch

After: `ps2x_tests` 507/507, end-to-end 2/2, hardware result tests 32/55 exact (6627/8250 lines), no
regressions. State and remaining failures by cause: `docs/hwtest/README.md`, `docs/hwtest/dashboard.json`.

- FPU: ADD.S/SUB.S (and the ACC/MADD/MSUB additions) round toward zero (an exact-rational model reproduces all 76
  hex-operand rows); denormal operands count as zero on the fast path too; C.EQ/C.LT/C.LE and MAX.S/MIN.S compare
  encodings (no NaN, denormal = 0, -0 < +0 for MAX/MIN); MADD/MSUB do not saturate an overflowing product before
  accumulating; FCR31 gets O/SO on saturating ADD/SUB/MUL, I/SI on 0/0 and SQRT of a negative, D/SD on x/0.
- MMI: PMULTW/PMULTUW/PMADDW/PMADDUW/PMSUBW (words 0 and 2, 64-bit HI:LO accumulators); 160 more real-PS2 vectors.
- Kernel: suspends do not nest; SuspendThread on the caller keeps it running and reads back as suspended until the
  next rescheduling point; StartThread warns when the entry was never recompiled.
- Recompiler: a code address whose LUI sits in a JAL delay slot of a callee-saved register is now found as an
  entry point (a thread entry in ps2autotests' delete.elf was missing, so StartThread failed).
- VU0: LQC2/QMTC2 leave VF0 alone; CTC2 write masks for STATUS (sticky flags), R (23 bits), CMSAR0/1 (16 bits)
  and FBRST (reset bits clear DE/TE); CFC2 of vi19/vi23/vi25 read the hardware values.
- Mutation-checked: RZ addition, PMADDW accumulator, delay-slot LUI discovery.
