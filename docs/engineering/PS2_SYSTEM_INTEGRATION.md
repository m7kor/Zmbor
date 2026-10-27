# PS2 system integration

Cross-device paths and what tests them today. A path is "covered" only by a test that runs the whole path.

| path | test today | state |
|---|---|---|
| VIF1 → VU1 (MSCAL) → XGKICK → GIF → GS | `ps2_vu1_tests.cpp` "MSCAL can start a VU1 XGKICK program and update GS VRAM" | unit level |
| EE → GIF DMA (PATH3) and VIF1 DIRECT (PATH2) → GS frame | e2e `gs_sprite` (whole frame compared) | e2e, synthetic program |
| fromSPR → MFIFO → GIF / VIF1 drain | `ps2_memory_tests.cpp` MFIFO tests | unit level (this branch) |
| EE → SIF → IOP (SIFCMD/SIFRPC/FILEIO) → host tty | every ps2autotests program prints through it | hardware programs, indirect |
| CDVD → IOP DMA → SIF → EE | none (CDVD is HLE at call level) | missing |
| EE → SIF → IOP → SPU2 | none (no SPU2 model) | missing |
| IPU → DMA → memory → display | none (IPU is HLE) | missing |
| SIO2 → pad / memory card | none (library HLE) | missing |
| DEV9 → DMA → IOP interrupt | none | missing |

Integration tests are added with the device model they exercise (waves 3-7 in `PS2_HARDWARE_DEPENDENCIES.md`).
