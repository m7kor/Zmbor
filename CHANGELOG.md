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

## Phase 3 — VU microcode execution (2026-10-23)

After: `ps2x_tests` 510/510, end-to-end 2/2; hardware result tests 38/55 exact (7394/8250 lines, was 6627),
no crashes, no regressions. Root causes, each found separately:

- Memory window (5 of 7 VU tests): EE accesses to VU0 micro memory (0x11000000) and VU1 data (0x1100C000) were
  outside the "special address" range and went to RDRAM at 0x01000000 / 0x0100C000, so microcode written by the
  EE never reached VU0 and VU1 results never reached the EE. Fixed in `Ps2IsPhysicalSpecialAddress`.
- VU1 start: CTC2 to CMSAR1 starts VU1 at that instruction (the tests' `ctc2 $0, $31`); before, only VIF1 MSCAL did.
- EFU crash: not VU emulation. Unoptimized hardware-test builds have ~0.7 MB host frames, so nested guest calls
  overflowed the 1 MB thread stack; those executables now reserve 64 MB.
- EFU arithmetic, rebuilt from 208 real-PS2 rows (167 exact; the rest differ in the last bits of the EATAN/ESIN/EEXP
  series): PS2 float decoding (exponent 255 is a number, +-max saturation), truncating multiply/add stages, divider
  and root to nearest, x/0 = +-max, roots of |x|, EATAN through t = (x - 1) / (x + 1) (an earlier change here had
  assumed the input was already t; hardware shows EATAN(1) = pi/4). EFU math runs to-nearest inside the
  interpreter's round-toward-zero mode, and its P result keeps exponent 255.
- VU FMAC addition drops an operand shifted out entirely (25+ exponent steps), whatever its sign (triace).
- Mutation-checked: VU window, vuAdd, EFU rounding scope.

## Phase 3 — VIF, SPR and DMAC (2026-10-23)

After: `ps2x_tests` 519/519, ctest 48/48 (research, GS cache, end-to-end), `ps2xIOP` 4/4; hardware result tests
47/55 exact (8172/8250 lines, was 38/55 and 7394), all 11 `dma/*` tests exact, no crashes, no timeouts, no
regressions. Root causes, by subsystem:

- Recompiler (dma/vif/stcycl timeout): a word that follows a branch was taken as a function start when a pointer
  table named it, so a delay slot became its own function. Starts in a delay slot are dropped.
- VIF UNPACK: VIF0 used a reduced unpacker (32-bit formats only, no S replication); both VIFs now share one.
  V2 writes x y x y, V3 takes w from the next element, V4-5 widens 5-bit channels to 8 bits, WL = 0 is 256 and CL = 0
  behaves as CL = WL. STMOD mode 3 writes the data unchanged and saves it into ROW.
- VIF commands split across transfers (STMASK/STROW/STCOL/MPG/UNPACK) were dropped; they now wait for the rest of
  their data with VPS = 1, and VIF1_NUM reads 256 for a pending NUM = 0 UNPACK (VIF0_NUM, 8 bits, reads 0).
- VIF registers: R0-R3 and C0-C3 were not readable.
- VIF I-bit stall: the DMA now completes when the held data fits the FIFO (VIF0 8, VIF1 16 qwords) and STAT reads
  VPS = 2. Before, any held data kept the DMA active (PCSX2's behaviour), which hardware contradicts
  (dma/vif/intstall); the unit tests that held a 1-qword DMA now use more data than the FIFO takes.
- SPR DMA: toSPR chain mode and interleave mode (TQWC/SQWC, 8-bit fields, MADR past the last skip) were not
  implemented. The source-chain walker shared by GIF/VIF is now one function, which also leaves MADR past the last
  tag's data.
- DMAC: BC0T/BC0F now test CPCOND0 (every channel selected by D_PCR.CPC has D_STAT.CIS); before, the branch was
  never taken.
- Mutation-checked: CL = 0, split UNPACK, VIF1_NUM, STMOD mode 3, R0-R3 reads, FIFO completion, VPS = 2, V2 lanes,
  toSPR chain, chain MADR, CPCOND0, interleave skip, BC0 emission, delay-slot starts.

## Phase 4 — hardware accuracy (2026-10-25, branch ultimate/phase4-hardware-accuracy)

Baseline re-measured on `00173a2` (Zmbor main, after the #384 merge): 47/55, 8172/8250 lines. After:
53/55 exact, 8202/8250 lines (99.4%), no crashes, no timeouts, no regressions; `ps2x_tests` 526/526, ctest 49/49
(with the new e2e_branchdelay), `ps2xIOP` 4/4 (Windows MSVC; Linux left to CI). Each fix has a regression test
and was mutation-checked.

- Recompiler, branch in a delay slot (cpu/ee/branchdelay, cpu/ee_fpu/branchdelay): the inner branch was
  translated as an ordinary instruction. It is now emitted as its own instruction and the outer transfer is
  modelled: an outer branch whose outcome is known early (jumps, branch-likely, beq rs,rs, b/bal) makes the
  instruction at its target the inner branch's delay slot; otherwise a taken inner branch replaces it. Fitted to the
  three all-taken hardware rows; register jumps and calls in this position fall back with a warning.
- Runtime memory ownership (memory/ee/default): the heap set up with size -1 now ends at the main thread's stack
  (30 MB fit, 32 MB do not, as on hardware). Runtime-owned allocations and the HLE RPC/TLS/boot-mode pools moved to
  the EE kernel area (0x00020000-0x00080000) so they cannot overlap the program's sbrk heap.
  When the arena is full, runtimeMalloc fails (HLE calls report failure) instead of borrowing guest-heap blocks the
  program's sbrk also hands out; an ELF segment inside that area disables the arena with a warning.
- COP0 performance counters: PCCR write mask, PCR0/PCR1 selection, and processor-cycle counting gated by CTE and the
  current mode's bit, from the scheduler's guest cycle count (not host time; not cycle-accurate).
- FPU MUL.S/MULA/MADD/MSUB: the product is truncated and the multiplier is not commutative (1.0 * 0x3FFFFFFF =
  0x3FFFFFFE), modelled as a Booth-recoded ft whose lowest negative digit misses its +1; the hardware rows do not
  distinguish this from wider truncated-array forms. About 1.46x the cost of the host multiply in isolation.
- VU0 vi24 keeps the written bits 0x0C0C (one hardware row).
- Not fixed: RSQRT.S (no simple rounding model fits all 39 rows) and the EFU series last bits.
