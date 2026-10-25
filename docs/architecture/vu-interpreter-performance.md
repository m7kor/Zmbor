# VU interpreter performance (Phase 6)

What the VU1 interpreter spends its time on, what was changed, and the evidence that the changes are equivalent.
Method: `docs/architecture/benchmark-methodology.md`.

## Workload

`tools/bench/vu1_interp_bench.cpp` (`ps2x_bench_vu1`), synthetic, written for this purpose (no game code):

- **transform**: per vertex LQ, MULAx/MADDAy/MADDAz/MADDw (4x4 matrix), DIV + WAITQ, MULq, FTOI4, SQ, two IADDIU,
  IBNE loop; 256 vertices, 8201 VU cycles per run. Every run's output is checked against an independent computation
  (`ps2_vu_fmac` integer reference, IEEE FDIV, FTOI4) before anything is timed.
- **reupload**: the same, with one code word rewritten through `PS2Memory::write32` before every run (the decode
  cache rebuilds, as after a VIF MPG upload).
- **xgkick**: 200 XGKICKs of a 2-qword GIF packet to PATH1, each waiting for the previous transfer.

It represents VU1 vertex/packet work; it does not represent a game frame (no VIF unpack, no GS drawing load).

## Profile (before)

`ps2x_bench_vu1 300 256 profile` in a Release build with `/Zi` (`build-prof`), sampling profiler
`tools/bench/sampling_profiler.cpp` (Windows, DbgHelp; self time, 250 us interval):

| function | self time |
|---|---|
| `commitReadyPipelines` | 48.6% |
| `calculatePairReadyCycle` | 13.4% |
| `run` | 11.0% |
| `execUpper` | 6.3% |
| FMAC value + flag helpers (all) | ~10% |

The interpreter ran at ~5.6 M VU cycles/s (433 ns per instruction pair). Decode is cached and FMAC arithmetic is
about a tenth of the time, so a static recompiler for VU arithmetic (the recvx-enhanced-port approach) would not
address the measured cost; the pipeline timing model's bookkeeping does.

## Changes (all keep the timing model and commit order)

1. **Commit index** (`m_nextReady`, `m_nextCommitCycle`): each pipeline (flags, Q, P, stores, VF, VI, ACC) keeps the
   earliest `readyCycle` of its pending entries; `commitReadyPipelines()` returns at once while nothing is due and
   scans only the pipelines with a due entry, in the original order, recomputing that pipeline's minimum in the same
   pass. Every queue site lowers its slot, so a slot can be too low (an extra scan) but never too high.
2. `calculatePairReadyCycle` iterates the set bits of `viRead` instead of 15 registers; the decoded pair is used by
   reference instead of copied every cycle; the 7-slot minimum is a plain loop.
3. **XGKICK**: `resetTransfer()` resets the transfer bookkeeping without clearing the 64 KB packet buffer; every byte
   below `copiedBytes` is written before it is read and only `totalBytes <= copiedBytes` are submitted.
4. `flushPipelines()` is bounded (a full-buffer XGKICK at 2 cycles/qword + 1024): an entry that never becomes due is
   reported and stops the program instead of hanging the host (found by mutation testing, below).

## Equivalence evidence

- Cycle counts are identical before and after (8201 transform, 33915 xgkick).
- `ps2x_tests` "VU1 commit index matches the full-scan reference on random programs": 3000 random programs (FMAC all
  operand kinds and ACC forms, FDIV/EFU with WAITQ/WAITP, flag ops, integer ops, LQ/SQ/LQI/SQI/LQD/SQD/ILW, MTIR/MFIR,
  forward branches, I-bit pairs), each run indexed and in reference mode (`setReferenceCommitMode`, the old full
  scan) from the same state; VF, VI, ACC, Q, P, I, R, PC, MAC, STATUS, CLIP, cycles, E-bit/branch state and VU data
  memory compared at a random cycle budget and at the end: 0 mismatches. Mutants (VF, flag and Q queue sites not
  indexed) are killed; before change 4 they hung the test instead of failing it.
- "consecutive XGKICKs deliver exactly their own bytes": a 4-qword packet then a 2-qword one, delivered bytes and
  sizes compared with the source; a mutant that keeps `copiedBytes` across transfers is killed.
- All existing VU, VIF, XGKICK and hardware tests (see the Phase 6 gate in `CHANGELOG.md`).

## A/B result

Five alternating rounds, each the median of 21 runs; binary A = the same benchmark source built against the runtime
of `0bac702`, B = the candidate; Intel Core i5-10400F, Windows 11, MSVC 14.44, `/O2 /Ob2`.

| scenario | A | B | speedup |
|---|---|---|---|
| transform, steady | 1453.1 us | 725.9 us | 2.00x |
| transform, reupload | 1514.0 us | 790.6 us | 1.91x |
| xgkick x200 | 672.2 us | 109.3 us | 6.15x |

Round-to-round spread: within 1% except one A xgkick round (775.7 us). The interpreter is still about 50x slower
than a real VU1 per cycle (~219 ns per pair); the remaining profile is spread over `run`, `calculatePairReadyCycle`,
`execUpper` and the FMAC flag path. No whole-game effect has been measured (no game workload is available).

## Not done (and why)

- Native VU microprogram specialization: not justified by this profile; it would also need content-identity
  invalidation (the generation counter alone is per-unit, not per-program) and a state-equivalence harness, which the
  reference mode and the differential test now provide for the scheduler part.
- The decode cache still rebuilds all 2048 pairs on any code write (reupload costs ~65 us here); a per-range
  invalidation is a candidate once a workload shows uploads to be frequent.
