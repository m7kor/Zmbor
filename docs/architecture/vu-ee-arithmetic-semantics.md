# EE and VU arithmetic semantics

How Zmbor represents PS2 floating-point arithmetic, and the rules every implementation of it must follow.
Evidence for each rule: `evidence-register.md` (sections 1 and 2). Probes that would settle open questions:
`hardware-probes.md`.

## 1. Layers

| layer | what it is | where |
|---|---|---|
| contract | the bit-level behaviour, stated per unit (EE COP1, VU FMAC, EFU, FDIV), with its evidence status | this document, `evidence-register.md` |
| reference | an implementation written directly from the contract in integer arithmetic, used only to check others | `ps2_vu_fmac.h` (`*Ref`), `tools/semantics/*.py` |
| lowering | what generated code and the interpreters execute; must equal the reference on the whole domain | `ps2_vu_fmac.h` (`add/sub/mul`), `ps2_runtime_macros.h` (`ps2_vadd` ...), `ps2_ee_fmul_bits` |
| verification | differential tests (lowering vs reference vs other execution paths), properties, hardware anchors | `ps2xTest/src/ps2_vu_fmac_tests.cpp`, `ps2_vu1_tests.cpp`, `ps2_float_semantics_tests.cpp` |

Rules:

1. A unit has **one** contract. Two execution paths of the same hardware unit (VU0 macro mode and VU0/VU1 micro
   mode run on the same FMAC design) must not implement different arithmetic.
2. The EE FPU and the VU are different units. They share code only where their contracts are the same; the EE COP1
   multiplier's operand-order asymmetry, for example, is not assumed for the VU.
3. A reference must not share code with the lowering it checks, and must not inherit its assumptions (the reference
   is integer arithmetic; the lowering uses exact double arithmetic).
4. Arithmetic results must not depend on the host floating-point environment: rounding mode, FTZ/DAZ, or compiler
   floating-point optimisation. See section 4.
5. A behaviour change needs evidence that discriminates it from the alternatives (A/B categories). Fitting known
   vectors is necessary, not sufficient: the model must also say what it predicts for inputs not yet measured.

## 2. PS2 float format (both units)

- Exponent 0 is zero whatever the mantissa (sign kept).
- Exponent 255 is an ordinary exponent: 0x7F800000 = 2^128, the largest magnitude is 0x7FFFFFFF.
- Results >= 2^129 saturate to ±0x7FFFFFFF.
- Results below 2^-126 become ±0, with the sign of the result.

## 3. VU FMAC contract (`ps2_vu_fmac.h`)

| rule | statement | status |
|---|---|---|
| A1 | both operands non-zero and exponents 25+ apart: the result is the larger operand unchanged | established (triace, E4) |
| A2 | otherwise the exact sum truncated toward zero; exact zero is +0 except -0 + -0 | established for truncation (triace, E2); shifted-out bits at gaps 1-24 provisional |
| S1 | SUB = ADD with ft's sign flipped | architectural |
| M1 | MUL: exact product truncated toward zero | provisional |
| MA | MADD/MSUB: product (M1) then A1/A2 with the accumulator: two truncations | provisional (micro interpreter, all VU tests pass) |
| F | flags (MAC, STATUS, sticky) | micro: computed; macro: not modelled (gap) |

### Lowering

- The micro interpreter (`ps2_vu1_upper.cpp`) calls the same `ps2_vu_fmac::addf/subf/mulf` at all 58 FMAC sites
  (ADD/SUB/MUL/MADD/MSUB with vt/bc/q/i operands, their ACC forms, OPMULA/OPMSUB) on the raw registers. Before
  Phase 5 it used host floats under `FE_TOWARDZERO` plus a local `vuAdd` A1 rule (OPMSUB lacked the A1 rule) and
  clamped exponent-255 operands and results to 0x7F7FFFFF. Unit tests compare micro mode with the integer reference:
  34,201 in-range plus 1,799 exponent-255 ADD/SUB/MUL results and 23,100 lane results over 39 other forms, all equal.
- Micro-mode flags (`fmacResultFlags`) classify an exact result taken at the contract's last rounding point: a
  product inside MADD/MSUB/OPMSUB is the contract's truncated product, a lone sum or product is exact. O at 2^129
  (format, provisional threshold), U below 2^-126, Z, S; product conditions go to the sticky flags. Flags never
  rewrite the value. Before the Phase 5 release review the flag path used the fused exact sum and overwrote the
  value on overflow/underflow/zero (MADD with acc = -2^128 and product 2^130 wrote 0x7FFFFFFF where the contract and
  macro mode give 0x7F7FFFFF), and the product sticky flags still clamped exponent-255 operands. Which flags the
  hardware raises in these cases is unmeasured (register 2.4).
- Macro mode (`PS2_VADD`/`PS2_VSUB`/`PS2_VMUL`, used by every COP2 FMAC macro) calls `ps2_vu_fmac::add4/sub4/mul4`,
  the SSE2 form of `add/sub/mul` (bit-identical on the whole test corpus, in all four host rounding modes, with and
  without DAZ/FTZ; 2.9–4.2× faster than calling the scalar kernel per lane, `benchmark-methodology.md`):
  - an exponent gap of 25 or more selects the larger operand;
  - otherwise operands are turned into doubles from their bits, added or multiplied exactly in double (a sum with
    a gap below 25 needs at most 49 bits, a product 48), and truncated back with integer operations.
  - Because no double operation in that path rounds, the result does not depend on the host rounding mode or on
    FTZ/DAZ. Exponent 255 needs no special path.
- Before Phase 5, macro mode rounded to nearest, had no A1 rule, and saturated EE-style. That legacy path is kept as
  `ps2_v*_legacy`, for measurement only.

## 4. Host floating-point environment

Policy (enforced or measured; ctest label `fpenv`):

| compiler | flags | status |
|---|---|---|
| MSVC | `/fp:fast /arch:AVX2` (release `EnableFastReleaseMode`) vs `/fp:precise` | **measured identical** for every production helper (EE ADD/SUB/MUL/DIV/SQRT/RSQRT/MADD, VU VADD/VMUL) over 1M operands (`fpenv_flag_independence`) |
| GCC/Clang | `-ffast-math` | **refused**: `ps2_vu_fmac.h` stops the build (`#error`, negative test `fpenv_fastmath_rejected`). Measured on CI before the guard: EE ADD.S/SUB.S/DIV.S/RSQRT.S/MADD.S results change; the VU FMAC paths do not |
| GCC/Clang | FMA contraction | `ps2_runtime` imposes `-ffp-contract=off` PUBLICly (recompiled code inherits it); the effect of contraction itself is not measured |
| GCC/Clang | `-O0` vs `-O3 -mavx2 -mfma` (contraction off) | compared by `fpenv_flag_independence` on CI |
| all | host rounding mode, DAZ/FTZ | the VU FMAC lowerings are independent of both (unit test over all four modes, with and without DAZ/FTZ). The EE helpers assume round-to-nearest; the VU interpreter sets FE_TOWARDZERO for its own code |

- The legacy (pre-Phase-5) macro path was flag-sensitive: with `/fp:fast` it gave a different result for the same
  operands inside a loop than in isolation (`e5eadc34` for `9ed6597e + 471482b6`, whose sum is `471482b6`).
- Code that calls `fesetround` must not rely on the compiler honouring it for constant folding (MSVC `/fp:precise`
  does not); keep such code free of foldable constants.
- No result above says shipped builds computed wrong PS2 results: the measured flag-sensitive cases were the legacy
  macro path and GCC/Clang `-ffast-math`, which no production configuration used.
- `EnableFastReleaseMode` is applied only for single-config `CMAKE_BUILD_TYPE=Release`; multi-config (Visual Studio
  generator) builds, including CI, use the default `/O2` flags.

## 5. EE COP1 contract (unchanged in Phase 5)

| op | contract | status |
|---|---|---|
| ADD/SUB | see evidence register 1.2: the documented "exact sum toward zero" is falsified (132/144); production effectively drops the smaller operand only past a gap of about 54; models with 0-3 guard bits fit 144/144 | unresolved |
| MUL | `ps2_ee_fmul_bits`: truncated, Booth digit-0 asymmetry | provisional |
| DIV | host division to nearest, x/0 rules | provisional |
| SQRT | sqrt of the magnitude to nearest | provisional |
| RSQRT | root to nearest, division to nearest (hardware differs on 7 rows) | unresolved |
