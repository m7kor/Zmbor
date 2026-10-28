# PS2 reference architectures

Sources consulted for hardware behaviour. They are references, not evidence: agreement between them is recorded as
such, and only real-PS2 output (ps2autotests, future probes) verifies a component. No code is copied; behaviour is
reimplemented in Zmbor's own structure, and each use is cited at the point of use.

| project | license | revision used | role |
|---|---|---|---|
| [PCSX2](https://github.com/PCSX2/pcsx2) | GPL-3.0 | `f0523f8acb1d` | primary behaviour reference (GS software renderer, DMAC, SPR, MFIFO) |
| [DobieStation](https://github.com/PSI-Rockin/DobieStation) | GPL-3.0 | `68dd073e7519` | independent second implementation (GS pixel pipeline, DMAC) |
| [Play!](https://github.com/jpd002/Play-) | BSD-2-Clause | `1f27232772e4` | third implementation, HLE BIOS and IOP design; no GS dithering |
| [ps2tek](https://israpps.github.io/ps2tek/) | documentation | read 2026-10-27 | register maps; does not define DIMX/DTHE, MFIFO or stall-control fields |
| [ps2autotests](https://github.com/unknownbrackets/ps2autotests) | ISC | `97469ffb` | real-PS2 expected output; the only hardware evidence used |
| [ps2sdk](https://github.com/ps2dev/ps2sdk) | AFL-2.0 | — | needed to build hardware probes; not installed |
| [parallel-gs](https://github.com/Arntzen-Software/parallel-gs) | LGPL-3.0 | — | candidate GPU backend; not integrated |

Zmbor is GPL-3.0 (from PS2Recomp), so GPL-3.0 sources are license-compatible; they are still not copied (see the
fuzzing and tool-evaluation documents for the reasoning).

## Per subsystem

### GS post-blend colour stage (dithering, COLCLAMP)
- PCSX2: `pcsx2/GS/GSRegs.h` (GIFRegDIMX, GIFRegDTHE), `GSState.h` `ExpandDIMX`,
  `Renderers/SW/GSDrawScanline.cpp` WriteFrame, `Renderers/SW/GSRendererSW.cpp` (sel.dthe, sel.colclamp, sel.fpsm).
- DobieStation: `src/core/gsthread.cpp` (DIMX register write, `dither_mtx`, COLCLAMP).
- Disagreements: D-GS-1.

### DMAC MFIFO and D_STAT
- PCSX2: `pcsx2/SPR.cpp` `_SPR0chain`, `pcsx2/Hw.cpp` `hwMFIFOWrite`/`hwMFIFOResume`, `pcsx2/Dmac.h` (D_CTRL MFD/STS/STD,
  D_STAT SIS/MEIS, RBSR/RBOR), `pcsx2/Gif.cpp`, `pcsx2/Vif1_MFIFO.cpp`.
- DobieStation: `src/core/ee/dmac.cpp` (`mfifo_handler`, SPR_FROM transfer, D_CTRL/D_STAT/RBSR/RBOR/STADR writes,
  stall-control checks).
- Disagreements: D-DMA-1, D-DMA-2.

## Adoption rules
1. Read the reference at a recorded revision; cite file and function where the behaviour is used.
2. Compare at least two implementations; record disagreements as decisions.
3. Write tests from the register definitions with hand-computed expectations, plus mutation checks.
4. Keep the component unverified until hardware output exists for its stated scope.
