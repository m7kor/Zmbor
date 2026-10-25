# Benchmark methodology

How performance claims in Zmbor are measured. A number without its harness, machine and run statistics is not a
result.

## Rules

1. **Equivalence before speed.** An optimised lowering (layer C) is only measured after it is shown bit-identical to
   the reference (layer B) on the shared test corpus; the benchmark itself re-checks the outputs of the variants it
   compares and refuses to report if they differ.
2. **Same inputs, same harness.** Variants run over the same operand vectors in the same process, interleaved per
   operation, so cache state, frequency and allocation are comparable.
3. **Inline as in production.** The operation under test is a template argument, so it inlines into the loop as the
   recompiled code inlines `PS2_VADD`. A first version passed a function pointer; that added an indirect call and the
   Windows x64 by-pointer vector passing to every variant and inflated all numbers (add4 17.8 ns instead of 13.6 ns).
4. **Statistics.** 3 warm-up runs, then 31 runs; median reported with min and max. A difference smaller than the
   min..max spread of either variant is not claimed.
5. **Report the regression too.** If correctness costs time against an older (wrong) path, the cost is stated.

## Tools

- `tools/bench/vu_fmac_bench.cpp` → `ps2x_bench_vu_fmac [runs]` (built with the tests, never run as a test).
- Google Benchmark, Tracy and LLVM-MCA were evaluated and **not** integrated: a 140-line harness answers the A/B
  question for header-only kernels; Tracy needs a running game workload (none available without commercial assets);
  llvm-mca is not installed (no clang toolchain here). The decision should be revisited for whole-frame profiling.

## Phase 5 result: COP2 macro VADD/VSUB/VMUL

Machine: the development PC of this phase (Windows 11, MSVC 14.44, `/O2 /Ob2`, Release). ns per 4-lane operation,
median [min..max] of 31 runs over 65,536 vectors.

| op | operands | legacy (pre-Phase-5, wrong contract) | lanes (Phase 5 first lowering) | sse2 (now) | lanes / sse2 |
|---|---|---|---|---|---|
| add | normal | 2.39 [2.38..3.81] | 33.03 [32.58..35.14] | 10.56 [10.48..10.87] | 3.13× |
| sub | normal | 2.40 [2.39..2.62] | 34.56 [34.43..41.53] | 10.70 [10.61..12.33] | 3.23× |
| mul | normal | 14.25 [14.14..16.05] | 28.04 [27.93..31.35] | 7.42 [7.40..7.56] | 3.78× |
| add | random bits | 3.91 [3.87..4.39] | 33.58 [33.36..36.90] | 11.31 [11.24..11.67] | 2.97× |
| sub | random bits | 3.92 [3.90..4.36] | 33.01 [32.83..33.57] | 11.44 [11.38..11.74] | 2.88× |
| mul | random bits | 35.58 [35.36..37.53] | 33.76 [33.68..34.13] | 8.12 [8.11..8.35] | 4.16× |

Steps, each measured: per-lane scalar kernel → SSE2 four-lane bit construction (2.4–3.6× over lanes) →
`__forceinline` (MSVC did not inline `add4` and passed vectors by pointer) → `CVTPS2PD` when no lane has exponent 0
or 255 (−22% add, −21% mul). MUL is now faster than the legacy path; ADD/SUB remain ~4.4× slower than the legacy
host add, which did not implement the FMAC contract (no gap rule, round-to-nearest, EE saturation). That cost is
accepted for correctness; no whole-game effect has been measured.
