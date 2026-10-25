# Architectural evidence register

Every behavioural model in Zmbor that is not plain MIPS/IEEE semantics, with the evidence it rests on.
Evidence categories (keep them apart; only A and B are hardware proof):

- **A** known real-PS2 vectors (ps2autotests `.expected`, the `*_hw_vectors.inc` files);
- **B** newly measured real-PS2 results (none yet — see `docs/architecture/hardware-probes.md`);
- **C** emulator reference results (PCSX2 etc.);
- **D** synthetic results derived from a specification or from a model;
- **E** Zmbor's own output.

Status: **established** (A/B evidence that discriminates the alternatives), **architectural** (follows from
documented hardware structure, not measured directly), **provisional** (fits the data, but the data does not exclude
alternatives), **unresolved** (competing models fit; no choice is claimed), **gap** (behaviour not modelled).

Tools for the arithmetic entries: `tools/semantics/` (exact rational/integer models, fitted against A).

---

## 1. EE COP1 (FPU)

### 1.1 Float format — established
PS2 floats have no Inf/NaN/denormal: exponent 0 is zero whatever the mantissa, exponent 255 is an ordinary
exponent (0x7F800000 = 2^128), results >= 2^129 saturate to ±0x7FFFFFFF, results below 2^-126 flush to ±0.
Evidence A: cpu/ee_fpu/{arithmetic,muldiv,sqrt,fcr}.expected (e.g. `mul CF_GARBAGE1, CF_GARBAGE2: 80000000` with a
denormal fs; `add CF_MAX, CF_MAX: 7fffffff`). Safe to generalise: yes.

### 1.2 ADD.S/SUB.S/ADDA/SUBA — the documented contract is falsified; the true model is unresolved
- Code comment (Phase 2) claims: *exact sum rounded toward zero*. Exact rational model M1 of that claim
  (`tools/semantics/fit_ee_add.py`): **132/144** hardware rows. The 12 failures are all opposite-sign operands with an
  exponent gap of 128 (`sub CF_MAX_EXP, CF_ONE: 7f800001`, M1 gives 7f800000): the hardware drops the smaller
  operand.
- The runtime passes those rows by accident: its "exact" sum is a double, which loses the smaller operand at gaps
  of about 54 and more. Its effective behaviour is M1 below a gap of ~54 and "larger operand" above.
- Alternative M2(g): align the smaller mantissa keeping g guard bits, drop the rest (no sticky), add, truncate:
  **144/144 for every g tested (0..3)**, and the data cannot bound g (every discriminating row has gap 128).
- Consequence: production, M2(0..3) and M2(29) all fit; they differ for opposite-sign operands with gaps of about
  24-53 and for shifted-out bits at smaller gaps. Discriminating probes: `hardware-probes.md` P1.
- Production is **not** changed (no evidence picks a model). The VU adder (2.2) drops at gap 25; whether the EE adder
  shares that design is a hypothesis, not evidence.

### 1.3 MUL.S/MULA/MADD/MSUB — provisional
Truncated product, non-commutative: `1.0 * 0x3FFFFFFF = 0x3FFFFFFE`, reversed operands exact (A, 72 rows).
Implemented (`ps2_ee_fmul_bits`) as a radix-4 Booth recoding of ft whose lowest negative digit misses its +1.
Wider truncated-array models (columns below K = 1..22 dropped) fit the same 72 rows. Prediction shared by all of
them: fs = power of two times ft with mantissa bit 1 set loses one ulp. They differ for other low partial-product
patterns (probes P2). Safe to generalise: the shared prediction yes; the exact form no.

### 1.4 DIV.S — provisional
Host division rounded to nearest, x/0 = ±max with D/I flags (A: muldiv, fcr). Only 4 DIV rows are inexact; one
(`div CF_ONE, CF_MAX_MANTISSA: 3f000001`) rounds up at 0.50 ulp. Nothing tests ties or other fractions.

### 1.5 RSQRT.S — unresolved (cpu_ee_fpu_sqrt 59/66)
Fitted on 39 rows: root rounded to nearest then truncated division 38/39; other root/division rounding choices
fewer. DIV.S rounds up at 0.50 ulp where an RSQRT quotient at 0.52 ulp does not, so the two do not share one simple
rule. Production unchanged; probes P3.

### 1.6 FCR31 flags — provisional
Only the cases in fcr.expected are modelled (overflow O/SO on saturation, D/SD for x/0, I/SI for 0/0 and sqrt of a
negative). Underflow and inexact are not modelled (gap).

## 2. VU0/VU1 arithmetic

### 2.1 Float format — established for EFU operands and results; architectural for FMAC
- VU results reach ±0x7FFFFFFF: vu/lower/efu.expected contains 0x7FFFFFFF 20 times and 0x7F7FFFFF never (A).
- Exponent-255 **operands** are values, not clamped: `ERSQRT CVF_MAX_EXP` (0x7F800001) gives 0x1F800000 = 2^-64
  = 1/sqrt(2^128), and `ERSQRT CVF_MAX` (0x7FFFFFFF) gives 0x1F3504F3 = 1/sqrt(~6.8e38); an operand clamped to
  0x7F7FFFFF would give ~0x1F7FFFFF in both (A). This is measured for EFU operand reads.
- Until Phase 5 the micro interpreter clamped exponent-255 FMAC operands (`normalizeOperand`) and results to
  0x7F7FFFFF, contradicting the format above. Since Phase 5 micro FMAC arithmetic reads raw registers and saturates
  at 2^129 like macro mode (`ps2_vu_fmac`); MAX/MINI/ABS/ITOF/FTOI still use the normalized operands (not audited).
  That the FMAC reads the register format as the EFU does is architectural (same VF registers), not measured on an
  FMAC row. The O flag threshold (2^129) follows from the format; no hardware row observes FMAC MAC flags.

### 2.2 FMAC ADD/SUB/MADD/MSUB — established for micro mode
- Truncation toward zero: experiment E2 (micro rounding switched to nearest) breaks vu/games/triace (3/5) and
  nothing else (A).
- An operand 25 or more exponent steps below the other is dropped, whatever its sign: experiment E4 (rule removed)
  breaks triace (4/5) (A).
- Below a gap of 25 the exact sum is truncated toward zero (`ps2_vu_fmac::addRef`); the data does not test
  shifted-out bits at gaps 1-24 with opposite signs (provisional; no probe generated yet).
- MADD/MSUB round twice (product truncated, then the gap-rule add); fits all VU tests, not isolated (provisional).

### 2.3 FMAC MUL — provisional
Exact product truncated toward zero. All VU tests pass with it; no row isolates it. Whether the VU multiplier has
the EE COP1 operand-order asymmetry (1.3) is untested (probe P4).

### 2.4 Macro mode (COP2 in EE code) vs micro mode — architectural; was a gap
VU0 macro instructions execute on VU0's FMAC, so they must follow 2.2/2.3. Until Phase 5 they did not: macro mode
rounded to nearest, had no gap rule, kept exponent-255 values while micro clamped them to 0x7F7FFFFF, and updated
no MAC/STATUS flags. Phase 5 puts both modes on one FMAC contract (`docs/architecture/vu-ee-arithmetic-semantics.md`): macro mode
through `ps2_vfmac`, micro mode through the same `ps2_vu_fmac` kernels at all 58 FMAC sites (unit tests compare 39
micro forms and plain ADD/SUB/MUL with the integer reference; mutants of OPMSUB, MADDAq, raw operands and the
overflow threshold are killed; a host-float product under FE_TOWARDZERO is an equivalent mutant in range).
There is no hardware vector for macro-mode arithmetic itself (cpu/vu0_macro covers integer ops and transfers only).
MAC/STATUS flags in macro mode remain a gap.

### 2.5 EFU — provisional (vu_lower_efu 182/223)
Stage model fitted to 208 vectors (167 exact); EATAN/ESIN/EEXP series differ in the last bits on 41 rows. The rounding
of series terms is unknown. Model families and a train/validation split: `hardware-probes.md` P5.

### 2.6 COP2 control registers — mixed
- Write masks of vi16/vi18/vi20/vi27/vi28/vi31 and the read-only registers: established, one row each
  (cpu/vu0_macro/transfer `ctc2 -1` rows) — each mask is the readback of an all-ones write, so other bit patterns
  are a prediction (safe for plain masks).
- vi24 keeps the written bits 0x0C0C: **provisional, one row**; whether it is a latch, a mirror of FBRST's DE/TE, or
  has side effects is unknown (probe P6).
- vi19 reads 0x2E30 (revision) whatever is written: one row.

## 3. Control flow

### 3.1 Delay slots, branch-likely annulment, links — established
Ordinary delay slots, likely annulment, BGEZALL/BLTZALL linking when not taken (A: cpu/ee/branch).

### 3.2 Control transfer inside a delay slot — provisional (three rows)
`beq` x4 -> 2, `bc1t` x4 -> 4, `bc1tl` x4 -> 2, all branches taken (A). Modelled as early-resolved (jumps, likely,
fixed-outcome encodings: the inner branch's delay slot is the instruction at the outer target) versus late (a taken
inner branch replaces the outer transfer). Alternatives not excluded: the split may depend on the BTB state or on
the unit (BC1 vs GPR) rather than the encoding. No row has a not-taken branch in this position. Probes P7.
Register jumps and calls in this position fall back with a warning (gap).

### 3.3 Scheduling and exceptions — gap
Guest exceptions are raised by the runtime only for a subset of cases (address errors on loads/stores, overflow
traps); there is no precise-exception restart model for branch delay slots beyond `ctx->in_delay_slot`/`branch_pc`.

## 4. Memory and loading — established/architectural
- Heap to the main stack (A: memory/ee/default), runtime arena and HLE pools in the EE kernel area, ELFs overlapping
  them refused (architectural: kernel memory on a real PS2), arena exhaustion fails cleanly. Tests and mutants:
  Phase 4.

## 5. COP0 performance counters — provisional/gap
PCCR mask, PCR selection, CTE/mode gating: established (A, one test). Only the processor-cycle event counts, from the
scheduler's guest cycle count (not cycle-accurate). Other events: gap.
