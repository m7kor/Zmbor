# License review for community sources

This fork is GPL-3.0 (inherited from `ran-j/PS2Recomp`, `LICENSE`). Licenses below are the SPDX ids the GitHub
API reports for each repository (`research/manifests/projects.json`, collected 2026-10-23); a file-level check
is still required before copying code from any file whose header says otherwise.

| Source | License | Can code flow into this fork? | Obligations |
|---|---|---|---|
| ran-j/PS2Recomp and its forks (Lautreck, LightVelox, drakolordx7, Sinan-Karakaya, silentsudin, SloppyWyn, BlackLineInteractive, ...) | GPL-3.0 | Yes | Keep authorship (cherry-pick, or `Co-authored-by` when adapting), keep notices. |
| Game projects: urban-reign-recomp, sotc-vibe-pc, BT3-Recomp, god-of-war-recomp, KillzoneRecomp, def-jam-vendetta-recomp, DQ8-Recompiled, RoadTripAdventure-recomp, SRWZ-native | GPL-3.0 | Yes, for engine code | Same. Game-specific code (addresses, CRC tables, title patches) is not imported into the engine regardless of license. |
| sal063/Ace-Combat-5-Static-recompilation | Apache-2.0 | Yes (Apache-2.0 code may be included in a GPL-3.0 work) | Keep its `NOTICE`/copyright and state changes, per Apache-2.0 section 4. |
| paraLLEl-GS (Arntzen-Software/parallel-gs, used by BT3 and Def Jam) | LGPL-3.0 | Yes as a separately built library linked into a GPL-3.0 program | Keep it a distinct component (submodule/library); review its Granite/Vulkan dependencies' licenses before shipping binaries. |
| PCSX2 | GPL-3.0 (API); per-file headers vary | Behaviour references are cited; code not copied so far | If code is copied: keep headers, verify each file's license. |
| Play! (jpd002/Play-) | BSD-2-Clause text in `License.txt` (API reports NOASSERTION) | Behaviour references are cited; code not copied so far | If code is copied: keep the copyright notice and disclaimer. |

Never imported, under any license: BIOS images, game executables, disc images, or other copyrighted game data.
Test data from community repositories is checked for this before reuse.
