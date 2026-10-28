# IPU semantics ledger

What the IPU model (`ps2xRuntime/src/lib/ps2_ipu.cpp`) does where it departs from ISO/IEC 13818-2 / 11172-2 semantics,
and what is not known about the PS2's own IPU.

**Evidence level:** no IPU behaviour has been checked on a physical PS2. The ffmpeg fixtures
(`tools/ipu/make_ipu_fixtures.py`) were decoded bit-exactly: 6 pictures, 13 824 samples, peak error 0. That shows the
model follows the standards' semantics. It does not show that it matches the PS2 IPU. Decisions D-IPU-1 to D-IPU-4 are
in `PS2_IMPLEMENTATION_DECISIONS.md`.

## A. Departures from the MPEG standards

| # | Where | Standard | Model | Why |
|---|---|---|---|---|
| A1 | BDEC DC predictors | Reset at each slice start, after a non-intra macroblock and after a skipped one (7.2.1). | Reset only by option DCR. | The IPU decodes one macroblock per command. Slice structure is software's job, and both references reset only on DCR. |
| A2 | BDEC non-intra output | Prediction plus residual (7.6). | The residual only (RAW16, signed). | Motion compensation is not an IPU function; EE software adds the prediction. |
| A3 | Intra RAW16 | IDCT output saturated to [-256, 255], then clamped at reconstruction (7.6.8). | Clamped to 0-255 at once. | Equivalent for intra blocks. PCSX2 clamps the same way. |
| A4 | IDCT | Any IEEE 1180 conformant IDCT. | Exact separable cosine transform in double precision, rounded to nearest. | The hardware's integer IDCT is unknown (D-IPU-3). |
| A5 | CTRL.IDP 3 | 11-bit intra DC precision (MPEG-2 4:2:2 and high profiles). | Supported: reset value 1024, multiplier 1. | ps2tek lists IDP 0-2 only, so the hardware's IDP 3 is unknown. |
| A6 | CTRL.IVF with CTRL.MP1 | MPEG-1 has no table B-15. | B-15 is used only when MP1 is clear. | Follows PCSX2. |
| A7 | Forbidden escape levels | 0 and -2048 (MPEG-2), and the MPEG-1 escape forms, are not allowed. | Decoded as they read; no error. | The PS2's reaction is unknown. |
| A8 | Coefficient run past index 63 | Not a valid stream. | CTRL.ECD; the macroblock produces no output. | The PS2's reaction is unknown. |
| A9 | coded_block_pattern 0 (`000000001`) | Not used with 4:2:0 in MPEG-2. | Accepted: all six blocks are 0. | The PS2's reaction is unknown. |
| A10 | macroblock_stuffing in MPEG-2 | MPEG-1 only (11-bit `0000 0001 111`). | Skipped in both. | PCSX2 does the same. |
| A11 | Slice ending in garbage (IDEC) | Not a valid stream. | CTRL.ECD (D-IPU-4). | PCSX2 sets no flag. |
| A12 | Quantiser matrix order | Always transmitted in zigzag order, whatever the scan (6.3.11). | As the standard. | PCSX2 weights by scan position, which differs under alternate scan (D-IPU-3). |

## B. Unknown PS2-specific IPU behaviour

Each item needs an IPU probe on a physical PS2. None of them is settled by the references or by the fixtures.

1. **Timing:** the cycles each command takes, and when BUSY, the IPU interrupt and the FIFO levels change while a
   command runs. The model advances only when it is run, and completes as soon as its data and output space allow.
2. **IPU_CMD value** after BDEC, IDEC, CSC and PACK. The model leaves it unchanged.
3. **IDCT rounding:** the hardware's IDCT may differ from the exact one by 1 in some samples (A4).
4. **Errors:**
   - how many bits the IPU consumes before it detects an invalid code;
   - whether it stops or carries on;
   - what a partly decoded macroblock outputs.
   The model stops at the invalid symbol without consuming it and outputs nothing for that macroblock.
5. **BDEC options outside their usual use:** DT with non-intra macroblocks, and DCR with non-intra macroblocks.
6. **IDEC at slice level:**
   - the DC reset after an increment above 1;
   - escape and stuffing in MPEG-2 streams;
   - a start code at a position that is not byte-aligned.
7. **CSC alpha thresholds:** PCSX2 zeroes the whole pixel below TH0, while the model zeroes only alpha (D-IPU-1).
8. **SGN with dithered RGB16:** the model applies SGN before the dither and the 5:5:5 reduction, as PCSX2 does.
9. **A command written while BUSY:** the model ignores it. Whether the hardware queues or overrides it is unknown.
10. **DMA chain modes:** chain modes of toIPU (channel 4) and fromIPU (channel 3) are not modelled; only normal mode
    is.
11. **VDEC/FDEC with fewer than 32 bits in the FIFO:** the model waits; whether the hardware returns early is unknown.

## C. Tests behind this ledger

All are in `ps2xTest`:
- **`ps2_memory_tests.cpp`:** hand-computed BDEC cases.
- **`ps2_ipu_mpeg_tests.cpp`:**
  - the ffmpeg fixtures for BDEC and IDEC;
  - pause/resume at every qword;
  - truncation;
  - CTRL.RST mid-slice;
  - corrupted streams;
  - the slice-syntax cases;
  - the toIPU → IDEC → fromIPU → RAM path.

Mutation testing: BDEC 15/15 mutants killed. IDEC results are in the PR.
