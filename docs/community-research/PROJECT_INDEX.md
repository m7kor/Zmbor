# Community project index

Collected 2026-10-23 with the GitHub API (`research/manifests/projects.json`, `research/manifests/forks.json`).
"Engine" is the PS2Recomp copy a game project builds against; ahead/behind are against `ran-j/PS2Recomp@main` (`eb8688b`).

## The 15 listed research targets

| Project | License | How it uses PS2Recomp | Engine vs upstream | Forensics |
|---|---|---|---|---|
| LightVelox/sotc-vibe-pc (Shadow of the Colossus) | GPL-3.0 | submodule LightVelox/PS2Recomp `main` | 46 ahead, 0 behind | `lightvelox-sotc.json` (36 records) |
| Sinan-Karakaya/DQ8-Recompiled (Dragon Quest VIII) | GPL-3.0 | submodule Sinan-Karakaya/PS2Recomp `main` (= upstream PR #254) | 139 ahead, 7 behind | `sinan-dq8.json` (48) |
| silentsudin/RoadTripAdventure-recomp | GPL-3.0 | submodule silentsudin/PS2Recomp `roadtrip` | 117 ahead, 3 behind | `roadtrip-srwz.json` |
| SloppyWyn/SRWZ-native (Super Robot Taisen Z) | GPL-3.0 | submodule SloppyWyn/PS2Recomp `srwz` | 1 squashed commit, 199 files | `roadtrip-srwz.json` (51 total) |
| drakolordx7/KillzoneRecomp | GPL-3.0 | submodule drakolordx7/PS2Recomp `killzone` + 26 patches + PCSX2 GS/microVU/IPU/SPU2 static libs | 2 ahead, 4 behind (+patches) | `game-projects.json` |
| z3xox/BT3-Recomp (DBZ Budokai Tenkaichi 3) | GPL-3.0 | vendored, heavily diverged; paraLLEl-GS submodule | not comparable | `game-projects.json` |
| KIexster/god-of-war-recomp | GPL-3.0 | upstream @ c5a9d02 + ordered patch stack (base: Waffle565 runtime) | patches | `game-projects.json` |
| thiagordsfx/urban-reign-recomp | GPL-3.0 | vendored TaylorNAlbarnaz/PS2Recomp `sotc-port`; PCSX2 GS DLL backend | not comparable | `game-projects.json` |
| jcsix694/def-jam-vendetta-recomp | GPL-3.0 | upstream @ 2c5fbb9 + 1 MB local diff + paraLLEl-GS diff | patches | `game-projects.json` |
| sal063/Ace-Combat-5-Static-recompilation | Apache-2.0 | independent Python recompiler + C runtime (name only) | n/a | `game-projects.json` |
| Lautreck/PS2Recomp | GPL-3.0 | fork; work lives in upstream PRs #372–#384 | identical to upstream | reviewed via upstream PRs |
| aslanhudajev/PS2RecompPlayground | GPL-3.0 | fork | 0 ahead, 50 behind | nothing to extract |
| dothacktranslate/PS2Recomp-dothack-recomp | GPL-3.0 | fork | 0 ahead, 5 behind | nothing to extract |
| BlackLineInteractive/PS2Recomp-Studio | GPL-3.0 | fork, archived | 4 ahead (Studio UI), 10 behind | not yet analyzed |
| TheGameMecha/HauntingGround | GPL-3.0 | fork | 0 ahead, 39 behind | nothing to extract |

## Discovered beyond the list (status DISCOVERED, not yet analyzed)

- 36 of 205 forks are ahead of upstream (`forks.json`). The largest are soyjxck/PS2Recomp (166 ahead),
  ZackLucas/PS2Recomp-GOW2 (112), hkmodd/RESWIII-PS2recomp (85), jrodales-dev (48), dustindustindustin (42),
  boubou666 (41, on the current base) and catsandsoup (12). MrCoolTheCucumber (546) is an old, 300+-file divergence.
- Non-fork projects found by search include HenryKun55/celosia (SotC for macOS/Vita), Waffle565/God-Of-War-Native-Port
  (the runtime God of War's patch stack is based on), literalynn/BurnoutDominatorReversed, DavvidMac/PS2Recomp-DevilKings
  (runtime fixes, Vulkan renderer), Gui-Tora/dmc-recomp, cxldsakeb/GoW2-nativo-PS2, koaque/Metropolismania-PC.

## Cross-project consensus

The same defect fixed independently in several engines is the strongest evidence that a fix is general:

| Defect | Projects that fixed it | Status here |
|---|---|---|
| SQRT.S read fs instead of ft | SotC, DQ8, Killzone, Def Jam, BT3, Road Trip | integrated (a3c7f21) |
| BLEZ/BGTZ/BLTZ/BGEZ compared bit 31 | SotC, DQ8, Def Jam | integrated (a3c7f21) |
| VU0 RSQRT/SQRT/DIV ignored fs / clamped / returned 0 | SotC, Killzone, Def Jam, DQ8 | integrated (252de7c) |
| CVT.W.S rounded instead of truncating | SotC, Killzone, Road Trip | integrated (252de7c) |
| LQ/SQ ignored the 16-byte alignment | SotC, Killzone | integrated (a42c052) |
| VF00 zero in new contexts | SotC, God of War, Road Trip | integrated (a42c052) |
| DMA chain cut at 4096 tags | SotC, DQ8, SRWZ | integrated (9ed5b6e) |
| Invocation stacks on the main thread's stack | DQ8, SRWZ, SotC | integrated (6460dd7) |
| VIF1 DIRECT pending IMAGE swallowed VIFcodes | SotC, DQ8, Killzone, SRWZ | integrated (c8093ef) |
| Whole-GS renderer behind a raw-GIF seam | BT3, Def Jam, Killzone, Urban Reign | design: `docs/decisions/0001-gs-raw-gif-seam.md` |
| VU1 microprograms compiled ahead of time | Def Jam, God of War, Urban Reign, BT3, AC5 | architecture reference |
| VU1 EFU sub-opcodes misnumbered (ERSQRT/ESIN/EATAN/EEXP) | DQ8, upstream PR #312 | integrated (3666b1b) |
| VIF I bit set STAT.INT but raised no INTC interrupt | DQ8, upstream PRs #367/#295 | integrated without the VIF stall (c8093ef) |

## Waffle565/God-Of-War-Native-Port (added in Phase 1)

| Branch | Revision | License | Relation |
|---|---|---|---|
| `main` | 514f89186f | none on the branch | docs, tools (zones.py harness), GoW overrides and configs; expects `runtime` in `ext/PS2Recomp-kl/` |
| `runtime` | ba16c8d70e | GPL-3.0 | history-preserving fork of ran-j/PS2Recomp at c5a9d02, 147 commits ahead; first commits apply KIexster/god-of-war-recomp's 68-patch stack |
| `switch` | bbf2b4b2be | GPL-3.0 | Nintendo Switch target derived from `runtime` |

Forensics: `research/candidates/waffle565-gow.json` (27 records). Integrated: PMULTH plus the rest of the MMI HI/LO
family (eff10b7), verified against PCSX2 and 160 real-PS2 result rows (d3765cf). Architecture references (not
integrated): an AOT R3000A IRX-to-C++ compiler (generator generic, override table game-specific), an AOT VU1
block recompiler driven by per-game captures, an OpenGL render-target GS backend with GoW-specific assumptions,
and the zones.py scene harness. Open candidates: MMI permutations/saturation (gow-03/04), VU0 R register (gow-05),
VIF STMOD 3 (gow-08), PACKED STQ Q latch (gow-09), IOP SIF GetOtherData (gow-11).

## Hardware reference data

unknownbrackets/ps2autotests (ISC): homebrew test programs with `.expected` output printed by a real PS2 for
EE/FPU/MMI/VU0-macro/IOP/VU instructions. Used for `ps2_float_hw_vectors.inc` and `ps2_mmi_hw_vectors.inc`.
Running its prebuilt ELFs end to end would validate whole instruction groups at once; that needs a decision about
running third-party prebuilt binaries (or a PS2SDK toolchain to rebuild them from source).
