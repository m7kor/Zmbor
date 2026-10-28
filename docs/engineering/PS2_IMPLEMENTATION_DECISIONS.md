# PS2 implementation decisions

Each record states the behaviour chosen where references disagree or where hardware behaviour is unknown, the
evidence for and against, and what would settle it. A decision is not hardware evidence; components that rest on
one stay unverified until the stated measurement exists.

## D-GS-1 — DIMX element encoding and which frame buffers are dithered

- **Chosen:** each DMyx is a 3-bit two's complement value (-4..3) added to R, G and B; dithering applies only to
  16-bit frame buffers (PSMCT16/PSMCT16S). `gs_cpu_backend.cpp` `ditherMatrixValue`, `WritePixel`.
- **For:** PCSX2 (f0523f8) `GSRegs.h` declares `s32 DMyx : 3`; `GSDrawScanline.cpp` WriteFrame adds the matrix only
  when `fpsm == 2` (16-bit formats). PCSX2 is the most widely tested implementation.
- **Against:** DobieStation (68dd073) `gsthread.cpp` reads bit 2 as "subtract (low two bits + 1)", so `100` is -1 and
  `111` is -4 (the reverse order for negative values), and dithers every frame-buffer format. Play! has no dithering.
- **Settles it:** GS output captured on a PS2 for a dither probe: flat-colour quads over a DIMX with distinct negative
  entries, in PSMCT16 and PSMCT32, read back through local-to-host transfers.
- **Status impact:** `gs.dither` is `IMPLEMENTED_UNVERIFIED`.

## D-GS-2 — COLCLAMP and the order blend → dither → clamp

- **Chosen:** blending and dithering produce signed values wider than 8 bits; COLCLAMP=1 saturates to 0..255,
  COLCLAMP=0 keeps the low 8 bits. COLCLAMP's reset value is 0; the SDK environment (`Support.h`) sets 1.
- **For:** PCSX2 WriteFrame (`colclamp == 0` masks with 0xFF, otherwise packs with saturation) and DobieStation
  (`COLCLAMP` clamps, otherwise `&= 0xFF`) agree, both after dithering.
- **Against:** none found.
- **Settles it:** GS output for blend probes that overflow and underflow with both COLCLAMP values.
- **Status impact:** `gs.alpha` stays `IMPLEMENTED_UNVERIFIED` (no GS hardware program covers blending).

## D-DMA-1 — When fromSPR writes into the MFIFO ring

- **Chosen:** fromSPR wraps into the ring only when D_CTRL.MFD selects a drain **and** its MADR lies inside
  [RBOR, RBOR + RBSR + 16).
- **For:** DobieStation wraps whenever MFD is set; PCSX2 `_SPR0chain` wraps whenever MADR is inside the ring, without
  looking at MFD. Requiring both never writes outside the ring when either reference would not.
- **Against:** a program that writes fromSPR into the ring with MFD=0 and expects wrapping (PCSX2 behaviour) would
  write linearly here.
- **Settles it:** an MFIFO probe on hardware writing into the ring with MFD=0 and with MADR outside the ring.

## D-DMA-2 — MFIFO stall granularity

- **Chosen:** the drain consumes a tag only when the tag and the data that follows it are in the ring; otherwise it
  stops before the tag.
- **For:** the runtime's DMA model transfers whole tags synchronously (`dma.timing` is HLE); the data a program sees
  after the drain resumes is the same.
- **Against:** the hardware (DobieStation `mfifo_handler`, per qword) can stop inside a tag's data, so MADR/QWC
  observed while stalled can differ.
- **Settles it:** an MFIFO probe that reads drain MADR/QWC while stalled; then a per-qword drain if they differ.

## D-DMA-3 — D_STAT bit 31

- **Kept:** bit 31 of D_STAT is this runtime's INT1 line (some masked status bit is set). The hardware register has no
  defined bit 31 (PCSX2 `Dmac.h` DMACh_STAT); a guest read sees it. Unchanged from earlier phases; noted so that it is
  not mistaken for hardware behaviour.

## D-DMA-4 — Stall control (D_CTRL.STS/STD, D_STADR)

- **Chosen:** with STS = fromSPR, a fromSPR normal or interleave transfer leaves its final MADR in D_STADR. The STD
  drain (VIF1 or GIF) checks `MADR + QWC * 16 > D_STADR` before each REFS tag and before a normal-mode transfer; if
  so it stops before the data, stays active (STR set, no CIS) and raises D_STAT.SIS once per stall. A write to D_STADR
  or D_CTRL, or a fromSPR transfer that moves D_STADR, makes it check again from where it stopped.
- **For:** PCSX2 (`Gif.cpp`, `Vif1_Dma.cpp`: REFS check, `tadr -= 16`; `SPR.cpp`: STADR = MADR in normal and
  interleave mode) and DobieStation (`dmac.cpp`: `can_stall_drain` for non-chain modes and REFS, `update_stadr`
  re-activates the stalled drain, `has_dma_stalled` raises SIS once) agree on the comparison, the source update and
  the REFS rule.
- **Against:** the references differ on granularity: PCSX2 stalls a whole tag, DobieStation per block of up to 8
  qwords, so MADR observed during a stall can differ from hardware (same limitation as D-DMA-2). Normal-mode drain
  stall follows DobieStation only (PCSX2 logs it as unimplemented). D_STADR updates during a fromSPR transfer are not
  visible because the transfer is synchronous. SIF0 and fromIPU as sources, SIF1 as drain and the fromSPR
  destination-chain CNTS tag are not modelled (those channels have no DMA model in `PS2Memory`).
- **Settles it:** a stall-control probe on hardware reading drain MADR/QWC, D_STADR and SIS while a fromSPR source and
  a GIF/VIF1 drain overlap, in normal mode and with REFS tags.

## D-IPU-1 — IPU colour-space conversion (CSC)

- **Chosen:** the integer BT.601 method with Y bias 16 and coefficients 0x95 (Y), 0xCC (R from Cr), -0x68 (G from Cr),
  -0x32 (G from Cb), 0x102 (B from Cb) in 1/64 units, each term shifted right by 6, summed, plus 1, shifted right by 1,
  clamped to 0-255. Alpha is 0 below TH0 in all channels, 0x40 below TH1, else 0x80. RGB16 takes the top 5 bits,
  optionally after a 4x4 dither, with bit 15 set for alpha 0x40.
- **For:** PCSX2 (`IPU/yuv2rgb.cpp` yuv2rgb_reference) describes this as the precise documented integer method used by
  the hardware. Its dither matrix and RGB16 packing (`IPU/IPUdither.cpp`) agree with DobieStation's.
- **Against:** DobieStation (`ee/ipu/ipu.cpp` process_CSC) uses full-range float coefficients (1.402, 0.34414,
  0.71414, 1.772) without the Y bias; the two give different colours. The alpha threshold rule is DobieStation's.
  The value read from an empty IPU_out_FIFO (zero here) is unknown.
- **Settles it:** an IPU probe converting known macroblocks (grey ramp, saturated chroma, values around TH0/TH1)
  and reading the RGB32/RGB16 output back.
