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
