# GS Backend Assessment — Audit, Measurement, and Interface Design

**Milestone:** GS backend assessment (assessment blocker 4 prerequisite).
**Baseline:** `350ca1f3b2cd2aede0d5ec8491b36d2c8a552141` (2026-10-28). Working tree at assessment time: 50 files vs baseline (Phases 3–15).
**Status: ASSESSMENT ONLY. No product code was changed, no test-tree file was changed, the working tree is untouched by this milestone, and NO patches are produced** (see §6). All measurements below were actually run; every number states its method and conditions. No commercial-game compatibility is claimed anywhere.
**Recommendation: PROVISIONAL — lean paraLLEl-GS adapter, staged behind the ADR 0001 seam and a differential gate** (see §4). The recommendation is provisional because the research program's graphics findings are Stage 1 only, the ADR seam is unimplemented, and no differential result against a second backend exists yet. Nothing in this report presents a provisional choice as final.

## 0. Corrections to the brief's premises (checked in code)

Four premises in the milestone brief were wrong or understated. Each correction changes what the implementation milestone must do.

1. **"No backend abstraction boundary is defined" — WRONG.** A real boundary exists: `GSRasterBackend` (`ps2xRuntime/include/runtime/gs/gs_backend.h`, 34 lines) with exactly one implementation, `GSCpuBackend` (`ps2xRuntime/include/runtime/gs/gs_cpu_backend.h`, `src/lib/gs/gs_cpu_backend.cpp`, 2,153 lines). The `GS` frontend (`ps2xRuntime/include/runtime/gs/gs_frontend.h`) owns it via `unique_ptr` and defaults to the CPU backend (`gs_frontend.cpp:108-111`); `setRasterBackend()` (`gs_frontend.cpp:1636-1664`) is the only replacement seam. What is missing is a *second* implementation, the raw-GIF seam for whole-GS renderers, and a differential harness — not the boundary itself.
2. **"No measured performance baseline exists" — was true; this milestone supplies one** (§2). The research program's open item ("GS draw time inside the gs_sprite total is not separately measured — UNKNOWN") is now answered for flat and primitive-heavy scenes. Textured/blended/feedback/CRTC fixtures remain unmeasured (see §2.5).
3. **The gs_sprite check is stronger than "one sprite frame" suggests — and narrower.** It is an exact full-frame RGB comparison (tolerance 0, all 640×448 pixels) via `check_frame.py` against `gs_sprite.frame.json` — not just the stdout line `E2E gs frame submitted`. But it proves only flat, untextured, unblended sprites (see §1.5).
4. **`docs/e2e-gs_sprite-frame.png` is NOT the oracle.** No file in the tree references it; `check_frame.py` compares the dumped PPM against the generated JSON expectations. The PNG appears to be a captured illustration only; whether it came from this program revision is UNVERIFIED.

## 1. Audit (checked in code, not from components.json)

`components.json` rates `gs.genregs`, `gs.vram`, `gs.swizzle`, `gs.xfer`, `gs.prims`, `gs.tex`, `gs.clut`, `gs.alpha`, `gs.tests`, `gs.fog`, `gs.mask`, `gs.dither` as `IMPLEMENTED_UNVERIFIED`, `gs.priv`/`gs.events` as `VERIFIED_PARTIAL`, and `gs.vulkan`/`gs.pgs` as `MISSING`. The code supports that shape, with the specifics below. Verdicts: **IMPLEMENTED** = code exists and a located test asserts it; **PARTIAL** = code exists but a named sub-behaviour is absent/simplified; **STUB** = no functional code found. Everything not proven by code or test is marked **UNVERIFIED**. No hardware claim is made.

### 1.1 GS register locations and the GIF path, hop by hop

**Privileged registers (display/CSR side)** live in `PS2Memory`, not the frontend: `GSRegisters` (`ps2xRuntime/include/runtime/ps2_memory.h:218-241`, 20×`uint64_t`: pmode, smode1/2, srfsh, synch1/2, syncv, dispfb1/2, display1/2, extbuf, extdata, extwrite, bgcolor, atomic csr, atomic vsyncTick, imr, busdir, siglblid) at base `0x12000000` (size `0x2000`). Address decode is `gsRegPtr()` (`ps2xRuntime/src/lib/ps2_memory.cpp:73-120`); CSR (`0x1000`) is excluded and handled via `writeCsrHalf`/`writeCsrFull` (write-one-to-clear bits 0–1). Direct guest CPU writes (`PS2Memory::write32` ~`1009+`, `write64` ~`1069+`, reads ~`793+`/`850+`) read/write `gs_regs` directly and **never forward into `GS::writeRegister`**. GIF_CTRL/GIF_MODE (`0x10003000`/`0x10003010`) are plain `m_ioRegisters` storage; GIF_STAT (`0x10003020`) is synthesised (`ps2_memory.cpp:3122-3134`). **UNVERIFIED:** no `0x10006000` GIF FIFO branch exists in `write128` (only VIF0/IPU/VIF1 FIFO branches); the disposition of a direct CPU store to the GIF FIFO was not fully traced.

**General registers (draw state)** live in the `GS` frontend (`gs_frontend.h:181-247`): `GSContext m_ctx[2]`, prim/PRMODE registers, current colour/ST/UV/fog, `m_prmodecont/m_pabe/m_scanmsk/m_dimx/m_dthe/m_colclamp/m_texa/m_texclut`, transfer registers, and a 6-entry vertex queue. Register IDs and decoded structs are in `gs_types.h:44-119`. Decode is `GS::writeRegisterUnlocked` (`gs_frontend.cpp:1069-1526`), handling PRIM, RGBAQ, ST, UV, XYZF2/3, XYZ2/3, TEX0_1/2, CLAMP_1/2, FOG, TEX1_1/2, TEX2_1/2, XYOFFSET_1/2, PRMODECONT, PRMODE, TEXCLUT, SCISSOR_1/2, ALPHA_1/2, TEST_1/2, FRAME_1/2, ZBUF_1/2, FBA_1/2, BITBLTBUF, TRXPOS, TRXREG, TRXDIR, HWREG, PABE, FOGCOL, TEXFLUSH, SCANMSK, DIMX, DTHE, COLCLAMP, MIPTBP1/2_1/2, TEXA, SIGNAL, FINISH, LABEL, plus raw `0x59/0x5a/0x5b/0x5c/0x5f` aliases writing priv-reg display fields. **Gap:** `GS_REG_AD (0x0F)` has no case (only the A+D packed descriptor `0x0E` is handled); a direct `writeRegister(0x0F,…)` falls to `default: break` and is silently dropped — harmless only because A+D is the real transport for AD.

**GIF packet trace:**
1. **PATH3 (GIF DMA, channel `0x1000A000`)**: `PS2Memory::startEeChannelTransfer` queues GIF transfers; `processPendingTransfers` (`ps2_memory.cpp:2447-2529`) copies data and calls `submitGifPacket(GifPathId::Path3, …)`, which honours PATH3 masking (`m_path3Masked` FIFO) then `GifArbiter::submit` (or legacy `m_gifPacketCallback`).
2. **PATH2 (VIF1 DIRECT)**: `submitPath2DirectPayload` (`ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp:647-675`, DIRECT/DIRECTHL at 882-895) wraps IMAGE payloads in a synthetic GIF tag and calls `submitGifPacket(Path2, …, directHl)`.
3. **PATH1 (VU1 XGKICK)**: `ps2_vu1_core.cpp:1034` calls `m_activeGs->processGIFPacket` directly, bypassing the arbiter.
4. **Arbiter** (`ps2xRuntime/include/runtime/gs/ps2_gif_arbiter.h`, `src/lib/gs/ps2_gif_arbiter.cpp:33-69`): `submit()` copies the packet, tags PATH3-IMAGE and PATH2-DIRECTHL; `drain()` stable-sorts (PATH3-IMAGE before PATH2-DIRECTHL, else path priority) and invokes `ProcessPacketFn = void(const uint8_t*, uint32_t)`. **The path ID is stored in `GifArbiterPacket.pathId` but is NOT passed to the callback — the frontend cannot tell PATH1/2/3 apart.** This is a gap the ADR seam must close.
5. **Wiring**: `PS2Runtime::syncCoreSubsystems` (`ps2xRuntime/src/lib/ps2_runtime.cpp:719-722`): `m_gs.init(gsVram, 4MiB, &m_memory.gs())`, arbiter callback → `m_gs.processGIFPacket(data,size)`, `m_memory.setGifArbiter(&m_gifArbiter)`. Runtime owns `GS m_gs` (`ps2_runtime.h:542`). Generated/recompiled guest code never calls the backend directly — it writes memory/DMA registers via `PS2Memory`.
6. **Frontend parse** (`GS::processGIFPacket`, `gs_frontend.cpp:641-739`): loops GIF tags (NLOOP bits 0–14, PRE bit 46 → PRIM, FLG bits 58–59, NREG bits 60–63 with 0→16, descriptors = 4-bit nibbles of tagHi). PACKED → `writeRegisterPacked` per qword; REGLIST → `writeRegisterUnlocked` per 64-bit word (+8-byte pad when `nloop*nreg` is odd); IMAGE/DISABLED (FLG 2/3) → `processImageData` → `m_backend->UploadImage`. Fast paths bypassing this loop: `tryProcessNativeImageUploadPacket` (setup A+D ×4 + IMAGE tag, `gs_frontend.cpp:809-878`) and `processNativePackedGIFPacket` (`740-779`, validated by `visitPackedGifPacket` at `50-96`), plus `PS2Memory::tryProcessNativeGifImageUploadChain` / `tryProcessNativeGifPackedChain` (`ps2_memory.cpp:2797-3047`) and the runtime native-GIF entry (`ps2_runtime.cpp:2573-2588`, called on GIF CHCR write).
7. **Packed vertex path** (`writeRegisterPacked`, `gs_frontend.cpp:879-1061`): descriptors `0x00` PRIM, `0x01` RGBAQ-split, `0x02` STQ, `0x03` UV, `0x04` XYZF (ADK bit 47 = no-kick), `0x05` XYZ (ADK), `0x0A` FOG, `0x0C/0x0D` XYZF3/XYZ3 (never kick), `0x0E` A+D → `writeRegisterUnlocked(addr, lo)`, `0x0F` NOP, default → `writeRegisterUnlocked(regDesc, lo)`.
8. **Draw kick** (`vertexKick(drawing)`, `gs_frontend.cpp:1528-1610`): vertex counts per type (point 1, line/linestrip 2, triangle/tristrip/trifan 3, sprite 2); XYZ2/XYZF2 kick, XYZ3/XYZF3 only queue (how strips/fans accumulate). On a completed drawing kick, `buildDrawBatch` (`1678-1703`: context by `prim.ctxt`, texa/texclut/pabe/scanmsk/dimx/dthe/colclamp/fog colour copied, texture size `1<<min(tw/th,10)`, `linearFilter` from TEX1 MMAG/MMIN bits) calls `m_backend->Submit(batch)`. Strips/fans are already expanded to one triangle per kick by the frontend queue, so the backend never sees strip/fan topology.
9. **Transfer/event fan-out from `writeRegisterUnlocked`**: TEX0/TEX2 → `m_backend->LoadClut`; TRXDIR → `m_backend->BeginTransfer`; TEXFLUSH → `m_backend->TextureFlush`; SIGNAL → `siglblid` low half + `csr|=0x1`; FINISH → `m_backend->Flush(); Sync(Finish)` + `csr|=0x2`; LABEL → `siglblid` high half; HWREG → `processImageData(buf, 8)` (8 bytes, not a full qword — a simplification).
10. **HLE path (not guest GIF)**: `ps2xRuntime/src/lib/Kernel/Stubs/GS.cpp` — `sceGsExecLoadImage`, `sceGsExecStoreImage`, `sceGsResetGraph` (GIF DMA via `0x1000A000` at 836-841), `sceGsSetDefDBuff/DBuffDc` (A+D packets built in guest memory) all synthesise GIF packets and drive the same DMA/frontend path; `sceGsSyncV` is a vsync wait, not a draw call.
11. **Presentation**: `GS::latchHostPresentationFrame` (`gs_frontend.cpp:527-576`) builds `GSPresentationRequest` from live `m_privRegs` (pmode/smode2/dispfb1/2/display1/2/bgcolor/vsyncTick) + context frames + preferred-source heuristic, then `Flush(); Sync(Presentation); Present(request)`; `copyLatchedHostPresentationFrame` (578-640) hands the latched RGBA frame to the host (raylib path in `ps2_runtime.cpp:396-430`, PPM dump via `PS2X_DUMP_FRAME`).

### 1.2 genregs — PARTIAL, not STUB

All registers in §1.1 are decoded and stored, including PRIM/PRMODE/PRMODECONT interaction (PRIM always sets type; attributes come from PRMODE when `PRMODECONT.AC=0`, `gs_frontend.cpp:1145-1165, 1286-1303`), TEX0/TEX2 field splits, FRAME/ZBUF/SCISSOR/ALPHA/TEST/FBA/BITBLTBUF/TRXPOS/TRXREG/TRXDIR, TEXCLUT/TEXA, CLAMP/MIPTBP stored raw, SIGNAL/FINISH/LABEL/CSR effects. **Decoded but never consumed by the rasteriser:** SCANMSK, MIPTBP1/2, TEX1 beyond linear-filter bits, PRIM.FIX. Stored-only is not implemented behaviour.

### 1.3 Textures, CLUT, transfers — IMPLEMENTED for the listed formats, PARTIAL overall

- **VRAM swizzle:** `ps2xRuntime/include/runtime/gs/ps2_gs_memory.h` (lookup-table `PixelStorageTraits`, `IsValidPsm` covering C32/C24/C16/C16S/P8/P8H/P4/P4HL/P4HH/Z32/Z24/Z16/Z16S) + `ps2xRuntime/src/lib/gs/ps2_gs_memory.cpp:223-398`. The CPU backend wires all 13 formats into 64-entry handler tables (`gs_cpu_backend.cpp:464-522`, default `ReadNull/WriteNull` = silent 0/no-op for any other PSM value, e.g. reserved codes).
- **Host→local upload** (`GSCpuBackend::UploadImage`, `gs_cpu_backend.cpp:1653-1759`): CT32/Z32 (4 B), CT24/Z24 (3 B), CT16/CT16S/Z16/Z16S (2 B), T8/T8H (1 B), T4/T4HL/T4HH (packed nibble pairs); any other `dpsm` hits `default: return` and silently stops the transfer. Upload runs only when `transferState.direction == 0`.
- **Local→local** (`PerformLocalToLocalTransfer`, 1760-1799) honours TRXPOS direction bits (bit 1 flips Y, bit 2 flips X). **Local→host** (`PerformLocalToHostTransfer`, 1800-1870 + `ConsumeLocalToHostBytes`, 1870-1881) packs by bits-per-pixel (32/24/16/8/4).
- **CLUT** (`LoadClut/LoadClutUnlocked`, `gs_cpu_backend.cpp:567-651`): only for indexed TEX0 PSMs; CLD 0/6/7 = no load, 1 = load, 2/3 = load + record CBP slot, 4/5 = conditional load on CBP change; CSM1 uses bit-3/4-swapped index (`swizzleClutIndexCSM1`, 291-296), CSM2 uses TEXCLUT CBW/COU/COV; 16-bit CLUTs fill 512 entries, 32-bit CLUTs split into two 256-entry halves; CSA masked to 0x1F (16-bit) / 0x0F (32-bit). Sampling lookup is `LookupCLUT` (1053-1095) with CSM1 8-bit block remap. **PARTIAL gaps:** CLUT is a snapshot copy (`m_clut[512]`, `gs_cpu_backend.h`), so later VRAM writes to the palette do not affect already-loaded draws until the next qualifying TEX0/TEX2 write — by design, but **UNVERIFIED** against hardware CLUT-load timing; TEXA is applied in `applyTexa` (65-95) only for CT24/CT16/CT16S (CT32 returned untouched) and only for non-CLUT texels in `SampleTexture` plus CLUT texels in `LookupCLUT` — no dedicated TEXA+indexed test line was found.
- **Sampling** (`SampleTexture`, 1096-1199): FST (UV/16) vs STQ (`S/Q·W`, `T/Q·H`, `fabsQ` guards Q≈0 → 1.0); CLAMP modes REPEAT/CLAMP/REGION_CLAMP/REGION_REPEAT via `wrapTextureCoordinate` (117-135); TEX0 TFX combine MODULATE/DECAL/HIGHLIGHT/HIGHLIGHT2 via `combineTexture` (241-289) with TCC selecting texture vs vertex alpha; point sampling vs bilinear (`linearFilter` only). **Mipmapping is STUB:** MIPTBP1/2 are stored (`gs_frontend.cpp:1431-1442`) and never read by the backend (0 hits in `gs_cpu_backend.cpp`); TEX1 LOD/K/LCM/MTBA fields are never read — only MMAG (bit 5) and MMIN (bits 6–8) distil to a single `linearFilter` bool (`gs_frontend.cpp:1697-1701`). `components.json` `gs.tex` itself lists "Mipmapping (LOD, MIPTBP) has no test" — confirmed by code, not just by the JSON.
- **Texture page cache** (`ps2xRuntime/include/runtime/gs/gs_texture_page_cache.h:11-37`): a **single** 8 KiB page mirror (`Resolve()` memcpys one page on miss, `Invalidate()` resets). Used only by texture/CLUT reads (`ReadTextureVramUnlocked`, `gs_cpu_backend.cpp:681-688`); framebuffer/draw writes go through the direct handler tables and bypass it. Invalidated on `TextureFlush` (657-661) and backend reset. Tests exist: `ps2xTest/gs_cache/gs_texture_cache_tests.cpp` (unaligned/wrap/stale-mirror/page-alternation/flush/upload/local-copy/raster visibility), `gs_clut_cache_tests.cpp`, `gs_memory_cache_tests.cpp`. **PARTIAL:** single-page cache means alternating two texture pages thrash (copied per access) — a performance property, not a proven output bug; stale-page risk is mitigated only by explicit TEXFLUSH/invalidation, so a texture overwritten without TEXFLUSH is **UNVERIFIED** (no invalidation on `WriteVram`/`UploadImage` was found in the backend — `TextureFlush` is the only invalidation call site besides reset).

### 1.4 Primitives, shading, blending, tests — mostly IMPLEMENTED, with named exceptions

- **Types:** all 7 `GSPrimType` values dispatch in `DrawPrimitive` (`gs_cpu_backend.cpp:724-860`): sprite → `DrawSprite` (1200-1315), triangle/tristrip/trifan → `DrawTriangle` (1316-1513), line/linestrip → `DrawLine` (1564-1635) / `DrawAntialiasedLine` (1514-1563), point → single `WritePixel`. **IMPLEMENTED** as far as dispatch goes; rasterisation quality is separate (§1.6).
- **Shading:** `DrawTriangle` interpolates RGBA barycentrically when `IIP` set, else flat from v2 (1390-1405); `DrawLine` interpolates or flat from v1; `DrawSprite` uses v1 colour only; point uses its vertex colour. Fog: `FGE` blends toward `FOGCOL` in `WritePixel` (869-884) with per-vertex fog interpolated on triangles/lines, v1 fog on sprites. **STUB for points/lines:** no texture sampling on point/line draws — `TME` is ignored there.
- **Blending:** `WritePixel` (862-1052): ALPHA A/B/C/D selectors, `(A−B)·C>>7 + D` on signed ints, PABE bypass when source-alpha MSB clear (974), FBA forces alpha bit 7 when `FBA=1` and PSM≠CT24 (1021-1024), FBMSK merge (1028-1031), COLCLAMP saturate vs low-8-bits wrap (1008-1019). **IMPLEMENTED** per the model; the signed-then-clamp/wrap order is explicitly a PCSX2/DobieStation-derived model choice, not a hardware capture (comment at 952-956).
- **Alpha test:** `passesAlphaTest`/`classifyAlphaTest` (143-210): ATE enable, ATST cases 0–7 vs AREF, AFAIL KEEP/FB_ONLY/ZB_ONLY/RGB_ONLY (RGB_ONLY degrades to FB_ONLY for non-CT32). **Destination-alpha test:** `passesDestinationAlphaTest` (211-229): DATE/DATM against bit 31 (CT32) / bit 15 (CT16/16S); CT24 and all other PSMs always pass. **Depth test:** ZTST bits 17–18 only — NEVER (0), ALWAYS (1), GEQUAL (2: `z >= stored`), GREATER (3: `z > stored`) at 934-947; `ZMSK` suppresses depth writes (1047), AA1 edge pixels never write depth (1047, `edge` flag). **GAP:** TEST bit 16 (ZTE, depth-test enable) is never consulted anywhere in `gs_cpu_backend.cpp` — a draw with ZTE=0 but ZTST=NEVER would still be depth-rejected by this code. Whether hardware gates ZTST on ZTE is **UNVERIFIED** from this tree, but the missing read is factual.
- **Scissor:** inclusive `[x0..x1]×[y0..y1]` check in `WritePixel` (865-867) plus bbox clamping in triangle (1344-1351) and sprite (1229-1237) paths. **SCANMSK is STUB:** decoded into `GSDrawState.scanmsk` (`gs_frontend.cpp:1689`) but never read by the backend (0 hits) — scanmask row skipping is not rendered.
- **XYOFFSET** applied (`>>4`) in point, sprite, triangle and line paths. **FBMSK** honoured in `WritePixel` and `ClearFramebuffer`.
- **Dither:** DTHE-gated, 16-bit framebuffers only, `DIMX[y&3][x&3]` 3-bit two's-complement (`ditherMatrixValue`, 43-53; applied 1000-1006 after blending, before COLCLAMP). The PCSX2-vs-DobieStation DIMX disagreement is documented in-code (comment at 38-42) — **UNVERIFIED** against hardware.
- **AA1:** implemented as this project's own model (decision D-GS-1): lines = two distance-weighted edge pixels per major-axis step (`DrawAntialiasedLine`), triangles = interior at alpha 0x80 + exact-area fringe pixels (`DrawTriangle` AA1 block, 1455-1512), coverage replaces source alpha (only when ABE clear or alpha==0x80), edge pixels skip Z writes. **PARTIAL/UNVERIFIED:** `components.json` `gs.aa` itself notes PCSX2 selects edge pixels differently and no hardware program checks AA1.
- **Framebuffer formats:** draw path supports any PSM with a handler-table entry; `WritePixel` converts 16-bit output via `Rgba8888ToRgba5551` and otherwise packs 32-bit. **Presentation (`CopyFrameToHostRgba`, 1941-2018) supports ONLY CT32/CT24/CT16/CT16S** — any other display PSM returns false (empty frame). **ClearFramebuffer (1882-1940) supports ONLY CT32/CT24/CT16/CT16S** and returns false otherwise. So an indexed/Z-format framebuffer can be drawn but not presented or cleared through these helpers.
- **Display/PMODE/interlace:** `PresentFromLocalMemory` (2033-2153): decodes PMODE (EN1/EN2/MM0D/AMOD/SLBG/ALP), SMODE2 interlace/frame-mode, DISPFB/DISPLAY (size fallback 640×448 when <64, clamped to 640×512 host buffer), dual-circuit merge with background colour, field line-doubling (`applyFieldPresentation`). A heuristic "preferred display source" (fullscreen textured sprite copy, `gs_frontend.cpp:1705-1736`) can override the DISPFB source, plus a fallback that substitutes a non-black context frame when DISPFB FBP 0 reads black (2058-2075). These heuristics are model choices, not decoded hardware behaviour — **UNVERIFIED** against hardware.

### 1.5 The gs_sprite e2e frame check — what it proves and does not

- **Program:** `ps2xTest/e2e/e2e_programs.py:403-529` (`program_gs_sprite`, registered at 628; CTest entry `ps2xTest/e2e/CMakeLists.txt:76` with `FRAME`).
- **What it renders:** two flat sprites, no texture, no blending, no dither, no AA. PATH3 GIF DMA (channel 2, normal mode, `0x1000A000` MADR/QWC/CHCR `0x101`) uploads a full-screen background sprite (colour `(0x20,0x40,0x80)`, FRAME_1 FBP 0/FBW 10/PSMCT32, SCISSOR 639×447, TEST ZTE+ALWAYS, ZBUF masked) via `gif_ad_packet` (PACKED A+D, `0xE`); PATH2 VIF1 DIRECT (channel 1, `0x10009000`, DIRECT opcode `0x50`) draws a red `(0xFF,0x00,0x00)` sprite over rect `(100,100)-(300,200)`. Display is set by direct priv-reg writes: PMODE `0xFF25`, DISPFB1 FBW 10/PSMCT32/FBP 0, DISPLAY1 MAGH 3/DW 2559/DH 447. It then prints `E2E gs frame submitted` and spins (`0x02000000` loop) so the host can present frames.
- **What the check compares:** `ps2xTest/e2e/run_e2e.cmake` runs the native executable with `PS2X_DUMP_FRAME=<file.ppm>`, requires exit 0 + the expected stdout line, requires the PPM to exist, then runs `ps2xTest/e2e/check_frame.py` against `<name>.frame.json` generated from `FRAME_EXPECTATIONS` (`e2e_programs.py:522-529`): width 640, height 448, `rects` = whole frame background + red rect, `tolerance: 0` — i.e. **exact per-pixel RGB comparison of the full 640×448 frame** (alpha is not in the PPM; `check_frame.py` compares only 3 channels). The script prints the SHA-256 of the pixel bytes but pass/fail is the rect comparison, not a hash match. The e2e source comment states the justification explicitly: "flat-coloured sprites at integer coordinates (no texture, blending, dither or AA) cover precisely x0 <= x < x1, y0 <= y < y1 under the GS fill rule, so no tolerance is justified."
- **What it proves:** end-to-end plumbing works for one frame — recompiled guest code → GIF DMA PATH3 + VIF1 DIRECT PATH2 → frontend register decode → CPU sprite rasterisation → priv-reg display setup → latched host presentation → PPM dump — with exact colours for two flat sprites, including scissor/framebuffer/display-size handling for this one configuration.
- **What it does NOT prove:** anything about textures, CLUTs, blending, alpha/depth tests (beyond ZTE+ALWAYS with masked Z writes), fog, dither, AA1, lines/triangles/points, strips/fans, mipmaps, interlace, PMODE dual-circuit merge, non-CT32 display, non-640×448 sizes, multi-frame animation, vsync timing, or performance. It also does not prove sprite edge-fill rules (the test deliberately uses interior geometry; its own comment says edge rules are "not under test here"). A single flat-sprite frame is consistent with a large number of rasteriser bugs elsewhere.

### 1.6 What the CPU rasteriser provably does NOT cover (gaps list)

1. No GPU backend at all (only `GSCpuBackend` implements `GSRasterBackend`).
2. No mipmapping / LOD (MIPTBP stored, never read; TEX1 LOD/K/LCM/MTBA never read).
3. No SCANMSK rendering (stored, never read).
4. No ZTE gating (TEST bit 16 never read).
5. No PRIM.FIX effect (decoded, never read).
6. Points and lines are never textured (`SampleTexture` is called only from `DrawSprite` and `DrawTriangle`); a `TME` point/line renders untextured. Same reason: no texture on AA1 lines.
7. Triangle rasterisation is float barycentric centre-sampling with a `1e-4` edge epsilon and degenerate rejection at `|denom| < 0.001` (`DrawTriangle`, 1353-1375) — not a fixed-point DDA; sub-pixel/edge-fill exactness vs hardware is **UNVERIFIED**. Sprite fill uses the `x1−x0` span rule with `max(1, span)` and inclusive clamped draw ranges.
8. Line rasterisation is Bresenham (`DrawLine`, 1578-1633) with linear `t` interpolation — not a hardware DDA model; **UNVERIFIED** for exact pixel coverage.
9. Depth precision: triangle Z is `double` barycentric then `+0.5` truncated to `u32`; XYZF Z is 24-bit, XYZ Z is 32-bit, both funnelled through the same `u32` write — Z-format conversion subtleties are **UNVERIFIED**.
10. HWREG transfers only 8 bytes (`gs_frontend.cpp:1501-1506`) per write, vs a 16-byte GIF qword — partial.
11. Presentation limited to CT32/CT24/CT16/CT16S and a hard 640×512 host buffer; display sizes <64 fall back to 640×448 and are clamped.
12. Texture cache is single-page + TEXFLUSH-only invalidation; correctness of sampling a texture overwritten without TEXFLUSH is **UNVERIFIED**.
13. Unknown PSMs silently no-op (handler tables default to `ReadNull/WriteNull`, `gs_cpu_backend.cpp:521-522`; `UploadImage` silently stops on unknown `dpsm`). Silent no-ops, not errors.
14. Performance at scene scale was unmeasured before this milestone (see §2 — now measured for flat/primitive scenes).

### 1.7 The existing backend boundary — real, and where it falls short for a GPU backend

`gs_backend.h` in full substance:

```cpp
class GSRasterBackend {
public:
    virtual ~GSRasterBackend() = default;
    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;
    virtual void Submit(const GSPrimitiveBatch &batch) = 0;                 // one decoded triangle/sprite/line/point
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;
    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;
    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;  // Finish | LocalToHost | Presentation | DebugReadback | Reset
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;
    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;
    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;
};
```

Data types are in `gs_types.h`: `GSPrimitiveBatch` (≤3 vertices + `GSDrawState`), `GSDrawState` (full context snapshot + prim/texa/texclut/pabe/scanmsk/dimx/dthe/colclamp/fog colour/texture size/linearFilter), `GSTransferCommand/Snapshot`, `GSPresentationRequest` (priv regs + context frames + preferred source), `PresentationFrame` (RGBA pixels + size + display/source FBP).

**Ownership split (as implemented):**
- **Frontend owns:** all guest-visible draw state (contexts, prim/PRMODE, vertex queue, current colour/ST/UV/fog, TEXA/TEXCLUT, transfer setup registers, priv-reg pointer, presentation latch + preferred-source heuristic, debug history). It is the only code that parses GIF tags, decodes registers, and decides when a primitive is complete.
- **Backend owns:** the VRAM byte array (shared external allocation — `Initialize(vram, vramSize)` takes a pointer it does not own; `GS::init` passes `PS2Memory`'s 4 MiB block), the CLUT snapshot (`m_clut[512]` + `m_clutCbp[2]`), the single-page texture cache, the transfer command/state + local-to-host staging buffer, and the PSM handler tables. It owns all pixel writes, texture sampling, transfers and presentation conversion.
- **Callers:** generated guest code never calls the backend directly. Host-side code calls `GS` methods (`latchHostPresentationFrame`, `ReadVram/WriteVram`, `clearFramebuffer*`, `consumeLocalToHostBytes`, `setRasterBackend`).

**Why the boundary is incomplete for a GPU backend:**
1. **No raw-GIF seam.** No `WantsRawGif/RawGif/VSync/GsReset` (§1.8); a GPU backend today must accept already-decoded batches and reimplement the §1.2–§1.4 pipeline itself, duplicating the frontend's decode — or the frontend must be extended.
2. **No path information.** The arbiter drops `pathId` before the callback (§1.1), so a backend cannot reproduce PATH1/2/3 arbitration or DIRECTHL semantics.
3. **No stream-order priv-reg capture.** Presentation reads live priv regs at latch time; a backend that renders asynchronously sees post-vsync values.
4. **No ordering contract in code.** `Flush/Sync` exist as no-ops in the CPU backend (`gs_cpu_backend.cpp:652-665`, comments "GPU backends may…"), but no documented guarantee that `ReadVram/SnapshotVram/ConsumeLocalToHost` observe all prior calls.
5. **Backend swap loses non-VRAM state.** `setRasterBackend` transfers only VRAM bytes; the CLUT snapshot, texture-page cache and transfer state are re-initialised empty, and the frontend does not re-issue `LoadClut` on swap. A mid-stream swap would render with an empty CLUT until the next qualifying TEX0/TEX2 write. (A test proves VRAM survives replacement: `ps2xTest/src/ps2_gs_tests.cpp:3173`, "GS backend replacement preserves canonical local memory" — one CT32 pixel round-trips. It does not cover CLUT/cache state.)
6. **No VERIFY/comparison mode.** No dual-backend page-hash diff exists; the only oracle is the CPU backend itself plus the single `gs_sprite` frame.
7. **Batch granularity is one primitive** (`Submit` per triangle/sprite/line/point, vertices ≤3) with a full `GSDrawState` copy per batch — a GPU backend would need batching/retention that the interface does not provide (no begin/end-frame, no command-list, no fence object beyond `Sync` reason codes).
8. **Presentation is host-RGBA only** (`PresentationFrame` pixels are already-converted RGBA); a GPU backend cannot return a native texture/swapchain image through this interface without changing it.
9. **No error channel.** Every method is void except `Present`/`ClearFramebuffer`/`ReadVram`/`ConsumeLocalToHostBytes`; a GPU backend has no way to report device loss, OOM, or shader failure except by crashing or silently producing wrong output.

### 1.8 ADR 0001 — `docs/decisions/0001-gs-raw-gif-seam.md`

Status is literally **"proposed (design only; nothing in this ADR is implemented yet)"**. It proposes adopting Urban Reign's minimal backward-compatible shape plus Def Jam's contract for whole-GS renderers that want raw GS input (motivated by paraLLEl-GS/PCSX2-GS integrations in BT3, Killzone, Urban Reign, Def Jam Vendetta):
1. `GSRasterBackend` gains defaulted virtuals `WantsRawGif()`, `RawGif(path, data, bytes)`, `VSync(const GSPrivRegs&, field)`, `GsReset()`; when raw mode is on, the frontend still parses tags and priv-reg writes but stops sending draws/CLUT loads/image data.
2. The GIF arbiter callback carries the PATH index (today `void(const uint8_t*, uint32_t)`).
3. The privileged register block is captured in stream order at each vsync.
4. A documented ordering contract: one producer; `ReadVram/SnapshotVram/ConsumeLocalToHost/Sync(LocalToHost)` observe every earlier call.
5. A VERIFY comparison mode: run CPU reference + second backend on the same stream, diff GS memory page hashes.
Consequences claimed: paraLLEl-GS (LGPL-3.0) or PCSX2 GS (GPL-3.0) could plug in without forking the frontend; no title-specific code behind the seam; each step needs tests keeping today's golden GS output byte-identical.

**Whether the code matches it: No — none of the five items is implemented.** `gs_backend.h` has no `WantsRawGif/RawGif/VSync/GsReset`; `GifArbiter::ProcessPacketFn` is still `void(const uint8_t*, uint32_t)` with no path; presentation still reads live priv regs (`gs_frontend.cpp:506-525`); no ordering-contract document or enforcement exists in the header; no VERIFY mode exists. The one partial alignment is that SIGNAL/FINISH/LABEL/CSR handling already lives in the frontend (`gs_frontend.cpp:1478-1513`) as the ADR's context section requires, and the decoded-work backend boundary (§1.7) already exists — but that boundary is exactly what the ADR says is insufficient for raw-GIF renderers. The ADR's own "nothing is implemented yet" disclaimer remains accurate.

## 2. Measurements (all actually run)

### 2.1 Method

A standalone benchmark harness (`/tmp/gs_bench/gs_bench.cpp`, preserved at `goals/zmbor-ps2-recompilation-engineering/hidden_files/gs_bench.cpp`) links the current-tree Release `libps2_runtime.a` and drives the `GS` frontend exactly as the runtime does: GIF A+D packets (PACKED, `0xE` descriptor, same construction as the gs_sprite e2e program) via `GS::processGIFPacket`, presentation via `latchHostPresentationFrame` + `copyLatchedHostPresentationFrame` with a `GSRegisters` block filled like the gs_sprite program (PMODE `0xFF25`, DISPFB1 FBW 10/PSMCT32/FBP 0, DISPLAY1 MAGH 3/DW 2559/DH 447). No window is involved: `Present()` returns pixels in memory, and that is what is timed — the raylib window-upload path is not measured and is out of scope for a backend comparison. Timing is `std::chrono::steady_clock` around the call loop; 100 untimed warmup reps precede each timed loop (this absorbs the frontend's capped debug logging — first 48 packets detailed — so timed reps contain no log I/O). Draw and present are timed separately because the CPU backend is immediate: `Submit` rasterises synchronously (`gs_cpu_backend.cpp:559-565`), while `Flush`/`Sync` are no-ops (`652-665`); `Present` snapshots the full 4 MiB VRAM and converts the display region.

Scenes (all deterministic; sprite/triangle positions and colours from a fixed-seed LCG):
- **S0 — gs_sprite equivalent:** setup registers + full-screen background sprite `(0x20,0x40,0x80)` + red sprite `(100,100)-(300,200)`, 640×448 PSMCT32. 200 timed reps.
- **S1 — 1,000 flat 32×32 sprites:** setup + background sprite + 1,000 pseudo-random sprites. 50 timed reps.
- **S2 — 1,000 flat triangles:** setup + 1,000 pseudo-random triangles (16–80 px sides). 50 timed reps.
- **Present:** `latchHostPresentationFrame` + `copyLatchedHostPresentationFrame`, 20 reps per scene.

Conditions: Release build (`-O2` via the project's Release config), VM CPU AMD EPYC 9D25, 4 vCPUs, headless (no DISPLAY needed — the harness presents to memory). Three runs were taken; the table gives the range across runs. The harness is NOT in the repo tree, so no tree change was needed for measurement.

### 2.2 Results

| Scene | Draw mean (µs/frame) | Present mean (µs/frame) | Frame total (µs) | Implied GS-only ceiling |
|---|---|---|---|---|
| S0 gs_sprite-equiv (2 sprites, 307k px) | 5,934–5,970 | 2,064–2,377 | ~8,000–8,350 | ~120–125 fps |
| S1 1,000 sprites 32×32 (~1.0M px writes) | 25,514–30,927 | 2,089–2,207 | ~27,600–33,100 | ~30–36 fps |
| S2 1,000 flat triangles (~0.8M px) | 31,230–32,364 | 2,141–2,277 | ~33,400–34,600 | ~29–30 fps |

Derived pixel rates: S0 ≈ 51M px/s; S1 ≈ 33–39M px/s; S2 ≈ 25–26M px/s (triangle edge-walking costs more than sprite fills). Present cost is nearly scene-independent (~2.1 ms: 4 MiB VRAM snapshot + RGBA conversion of 640×448), so it is a ~2 ms floor on every frame regardless of backend draw speed, unless a future backend presents from GPU memory without a full readback.

### 2.3 Correctness anchor (the harness reproduces the golden frame)

After S0, the latched frame (RGBA, 640×448, alpha 255) was scanned exhaustively: red pixels = **20,000**, bounding box exactly **(100,100)–(299,199)**, other-colour pixels = **0**; sampled pixels `(0,0) = (32,64,128)`, `(150,150) = (255,0,0)`, `(400,300) = (32,64,128)` — identical to the e2e golden frame (the e2e dumped PPM has exactly 2 colours, 266,720 background + 20,000 red). The harness therefore measures the same code path the e2e check verifies. One harness pitfall is recorded for future tool authors: the latched presentation frame is **RGBA (4 bytes/px)**; sampling it with a 3-byte stride produces garbage colours. The e2e PPM dump converts RGBA→RGB; the dump path, not the latch path, is what `check_frame.py` sees.

### 2.4 What the numbers mean for a 60 fps budget (16.7 ms/frame, GS alone)

- A trivial scene (S0 class) fits comfortably: ~8 ms GS-only, leaving ~8.7 ms for guest CPU, VU, SPU2, DMA, and host presentation before the frame budget is gone.
- A modest scene (1,000 sprites or triangles) already exceeds the budget on GS alone (~28–35 ms), before any other subsystem runs. The CPU rasteriser is therefore adequate for trivial scenes and the bottleneck for anything scene-like. A GPU backend's win on draw for primitive-heavy scenes is plausibly an order of magnitude (draw-bound, not present-bound — but see below), which is the quantitative case for doing the backend work at all.
- Present (~2.1 ms) is the floor: a GPU backend that still readbacks the full frame to host RGBA keeps this cost; only direct GPU presentation removes it. Any backend claim must therefore separate draw time, readback time, and present time — a single "fps" number hides exactly this split.
- The e2e gs_sprite wall time (4.25 s for the `e2e_gs_sprite` CTest, measured) is **NOT a GS measure**: it is dominated by the guest's `0x02000000`-iteration wait loop plus process start-up and several presented frames. Do not quote it as graphics performance. (The research program's 5.16/5.23/5.61 s figures for the same test are the same shape of number with the same caveat.)

### 2.5 What was NOT measured (and must be, before a final backend choice)

- **Textured scenes** (any PSM, CLUT, bilinear, TFX): no fixture exists yet. The single-page texture cache's thrash behaviour under alternating pages is unmeasured.
- **Blended / depth-tested / fog / dither / COLCLAMP scenes**, render-to-texture + CLUT-in-VRAM feedback, interlaced/FFMD/CRTC modes, and a 10⁴–10⁵-primitive large scene.
- **CPU rasteriser cost relative to the rest of a generated program's frame**: not obtainable from the current corpus without profiling hooks in generated code (the e2e wall time is wait-loop-dominated; the harness measures GS in isolation by construction). Separating guest-CPU cost from GS cost inside a real generated program is follow-up work (generated-code profiling, cf. the research program's category E), not something this milestone can report.
- **GPU submission/readback costs**: unmeasurable without a GPU backend; a Zmbor result where the CPU backend wins on small scenes (submission + readback dominating) would falsify the performance case for those scenes and must be checked by the implementation milestone's E2e gate, not assumed away.

## 3. Interface design (concrete, minimal)

Two seams, not one. The decoded-batch seam exists and stays; the raw-GIF seam is added for whole-GS renderers. Neither seam is a renderer.

### 3.1 Seam A — the existing decoded-batch seam (keep, document, close its gaps)

`GSRasterBackend` stays the interface for batch-oriented backends. The implementation milestone's first step (CPU-only, no behaviour change) must write down and close these contract gaps, each with a test:

1. **Ownership/lifetime.** Frontend owns all guest-visible state and the VRAM allocation; the backend borrows the VRAM pointer from `Initialize` and must not free it. `Reset()` re-initialises backend state (CLUT, cache, transfer state) but not the VRAM allocation. `setRasterBackend` transfers VRAM bytes only — document this, and either (a) have the frontend re-issue the last `LoadClut` and re-establish transfer state on swap, or (b) extend the swap to transfer backend state explicitly. Extend the existing backend-replacement test (`ps2_gs_tests.cpp:3173`, currently VRAM-only) to cover CLUT/cache state across a swap. Until this is fixed, mid-stream backend swaps are unsafe for textured rendering.
2. **Ordering contract (ADR item 4, written down, not just intended).** Single producer (the frontend). After `Sync(reason)` returns for `Finish`, `LocalToHost`, or `Presentation`, every prior `Submit`/`LoadClut`/`BeginTransfer`/`UploadImage` must be visible to `ReadVram`, `SnapshotVram`, `ConsumeLocalToHostBytes`, and `Present`. The CPU backend satisfies this trivially (immediate execution); a GPU adapter must fence here. `Sync(DebugReadback)` and `Sync(Reset)` carry the same visibility rule for their callers. This contract is what makes the VERIFY harness meaningful: without it, a page-hash diff cannot distinguish a backend bug from a sync race.
3. **Error channel (defaulted, non-breaking).** Add a defaulted virtual such as `virtual const char* LastError() const { return nullptr; }` (exact name to be settled by the implementation milestone; the requirement is a query, not a throw, because the frontend calls backends from contexts where an exception would cross the GIF path). The CPU backend returns none. A GPU backend reports device loss, OOM, and shader failure here; the frontend surfaces it through the existing debug/error path instead of the backend crashing or silently producing wrong output. No existing method signature changes.
4. **Batching discipline.** Keep per-primitive `Submit` for v1 — do not redesign the batch type yet. A GPU adapter batches internally and flushes on `Flush`/`Sync`/`Present` (those are already the flush points; document that `Flush()` means "make prior work visible to a subsequent `Sync`", not "present"). If a later backend needs begin/end-frame markers, they are added as defaulted virtuals, never by changing `Submit`.
5. **Presentation stays host-RGBA for v1.** `PresentationFrame` (RGBA pixels + size + display/source FBP) is the readback path and stays working for debug UI, frame dumps, and `check_frame.py`. Native-texture/swapchain presentation is a later interface extension, explicitly out of v1: it changes the host backend, not just the GS backend, and must not be smuggled into the adapter milestone.
6. **Guest-visible state never moves into the backend.** SIGNAL/FINISH/LABEL/CSR, PATH3 masking, DIRECTHL/IMAGE ordering, and the GIF arbiter stay in the frontend. Any adapter that moves them into the GPU is wrong (research program's guardrail, adopted here).

### 3.2 Seam B — the ADR 0001 raw-GIF seam (for whole-GS renderers)

Add to `GSRasterBackend`, all defaulted so the CPU backend and all existing callers are unaffected:

```cpp
virtual bool WantsRawGif() const { return false; }
virtual void RawGif(uint32_t path, const uint8_t *data, uint32_t sizeBytes) { /* no-op */ }
virtual void VSync(const GSPrivRegs &privRegs, uint32_t field) { /* no-op */ }
virtual void GsReset() { /* no-op */ }
```

(`GSPrivRegs` = the priv-reg block type; the implementation milestone settles whether it aliases `GSRegisters` or a snapshot struct. The requirement is a *snapshot*: captured in stream order at each vsync, never a live pointer the backend reads later.)

Frontend behaviour when a backend reports `WantsRawGif() == true`:
- The frontend still parses GIF tags and applies priv-reg writes (guest-visible state stays in the frontend), but suppresses `Submit`, `LoadClut`, and image-data delivery to the backend. Raw packets go to `RawGif` with the PATH index (this requires the arbiter change below).
- At each vsync, the frontend calls `VSync` with the stream-order priv-reg snapshot and the field index.
- `GsReset()` is called on frontend reset instead of (or in addition to, documented) `Reset()` for raw backends; the exact pairing is settled by the implementation milestone and tested by the null-backend test below.

Arbiter change: `GifArbiter::ProcessPacketFn` becomes a callback carrying the PATH index (e.g. `void(uint32_t path, const uint8_t*, uint32_t)`); the existing frontend entry keeps its signature via a wrapper, and the frontend forwards the path to `RawGif`. PATH3-IMAGE vs PATH2-DIRECTHL arbitration order in `drain()` is unchanged.

**Staging (CPU-only first, mandatory):** the seam lands against the CPU backend only, proven by (a) byte-identical `gs_sprite` golden output with the seam present-but-disabled (CPU backend reports `WantsRawGif() == false`), and (b) a null raw-GIF backend (records packets, changes nothing) that also leaves the golden output byte-identical. Only after both pass does any real adapter attach. If either fails, the seam work stops — it does not proceed to an adapter on a broken seam.

### 3.3 Differential-test harness design (VERIFY)

**Offline harness (no product changes — build first).** Generalise this milestone's `/tmp` harness into a tool that:
- reads a stream file: an ordered sequence of {GIF packets (raw bytes), priv-reg writes, present markers} — the same inputs the frontend consumes;
- drives a `GS` frontend instance with a chosen backend;
- dumps each presented frame (RGBA→RGB PPM, reusing `check_frame.py` semantics) and a VRAM page-hash list per frame (e.g. SHA-256 per 8 KiB page, or per 256-byte row — granularity settled by the implementation milestone; pages match the texture-cache page size and the ADR's "diff GS memory page hashes" wording);
- runs the same stream through the CPU backend (golden oracle) and through a candidate backend, and diffs frames + page hashes.

The CPU backend's golden outputs for every fixture are committed as expectations. Tolerances are **pre-registered per fixture before running the candidate** (see §3.4); adjusting a tolerance after seeing a diff invalidates the run.

**Fixture set (E1-complete; this milestone supplies only the flat/primitive part):**
- F-flat: gs_sprite-equivalent + multi-sprite flat scenes (tolerance 0, page-hash identical).
- F-tex: textured scenes across PSMCT32 + PSMT8/PSMT4 with CLUT (CSM1 + CSM2), point and bilinear filtering, all four TFX modes (bounded per-channel tolerance, pre-registered).
- F-blend: blended/depth-tested/fog/dither/COLCLAMP scenes (bounded tolerance).
- F-feedback: render-to-texture + CLUT-in-VRAM feedback (page-hash agreement required — this is the aliasing case the VRAM-exact model must win).
- F-crtc: interlaced/FFMD/overscan CRTC modes (field order, no vertical shift — checked as exact frame geometry, not pixel tolerance).
- F-large: a 10⁴–10⁵-primitive scene (throughput only: draw vs readback vs present separated; no pixel oracle beyond a coarse hash, to keep the fixture cheap).

**Optional in-frontend stream recorder (later, minimal hook, behind a flag).** If hand-written fixtures prove insufficient for real-program streams, add a recorder to the frontend: when enabled (compile-time or runtime flag, default off), it appends raw GIF packets + priv-reg writes in stream order to a file. Justification required before adding it: a named real-program stream that cannot be expressed as a hand-written fixture. When disabled, the only cost is one branch per packet; byte-identical output with the flag present-but-disabled is proven by the gs_sprite golden test. The offline harness does not need the recorder and must not wait for it.

### 3.4 Tolerance policy (pre-registered, non-negotiable)

- Flat/untextured/unblended geometry: **tolerance 0** (exact RGB + page-hash identical). This is the gs_sprite standard, extended to F-flat.
- Textured/blended output: **bounded per-channel tolerance, written down before the candidate runs**, citing the known non-bit-exact caveat for interpolation (paraLLEl-GS authors state hardware bit-wise accuracy on varying interpolation is not currently aimed at; AA1 handling is "questionable"). A tolerance widened after seeing a diff is not evidence.
- Feedback/aliasing: **page-hash agreement** (a wrong page is a wrong render, not a rounding difference).
- CRTC/field behaviour: **exact geometry** (field order, no vertical shift); a shifted frame fails regardless of pixel tolerance.
- A candidate that fails F-flat, or fails a whole format class systematically for adapter-attributable reasons, or fails F-feedback through its page tracker, **stops** — it does not become a fork, and its failures are not tuned away by widening tolerances.

## 4. Recommendation (PROVISIONAL)

### 4.1 The choice, and why it is provisional

**Provisional lean: paraLLEl-GS adapter behind Seam B, after the seam lands and the E2 gate passes.** Bespoke Vulkan-from-scratch is DEFERRED; PCSX2 GS is STUDY-as-reference / REJECT-as-import. The recommendation is provisional because every verdict that matters is gated on experiments not yet run (§4.4); the research program's own verdict for paraLLEl-GS is PROTOTYPE, not ADOPT, and this assessment concurs.

**Why the adapter leans ahead (evidence, not reputation):**
- It is a from-scratch Vulkan **compute** GS (triangle setup + binning + ubershader rasteriser + VRAM upload/texture-cache/CLUT-write/VRAM-copy shaders + page tracker), not a GSdx derivative — so it does not inherit GSdx's render-target approximation family.
- Its **VRAM-exact model** (rather than host-render-target approximation) is the single strongest technical reason for a static-recompilation runtime, where guest code routinely aliases framebuffers and textures. This maps directly onto Zmbor's F-feedback fixture class, which is where a conventional HW renderer is weakest.
- It already handles 2×/4×/8×/16× SSAA, weave de-interlacing, FFMD=0 progressive promotion, CRTC field blending, AA1, FSR1+RCAS — breadth a bespoke backend would have to re-derive.
- Licence: **LGPL-3.0-or-later**, compatible with GPL-3.0 Zmbor as a separately built library linked into a GPL-3.0 program (research verdict, matching Zmbor's own prior `LICENSE_REVIEW.md`), provided it is **not** vendored/copied into Zmbor's tree as if GPL-3.0 and LGPL obligations (library-source availability, relinking ability, notices) are honoured on distribution. Transitive check still owed (see §4.4).
- Pin: commit `cc6184af7e0c03da603045ca371ffa5dae9b0655` (HEAD at clone time, 2026-10-03; corroborated as a known-good pin by the DW5XL project). No stable API or versioning promise — pinning is mandatory. **Naming correction:** the old `Themaister/parallel-gs` path is not anonymously cloneable (prompts for credentials; likely renamed/moved); the current home is `https://github.com/Arntzen-Software/parallel-gs`. Any Zmbor doc citing the old path must be corrected.

**Why not bespoke Vulkan:** DEFER. The tooling is the easy 5% (volk MIT `aa76c86…` verified; Vulkan-Headers/VMA licences still provisional); the other 95% — GS-VRAM swizzle for every PSM, all texture formats + two-stage CLUT + mipmaps, all primitives + TEST/ALPHA/fog/dither/COLCLAMP/FBMASK/PABE/SCANMSK/AA1, GIF parsing, BITBLTBUF transfers, CRTC scanout — is a multi-month specialist project with near-certain early aliasing/feedback bugs, competing directly with higher-ranked recompiler work. Revisit only if the adapter fails for LGPL/policy reasons or on a mapping a bespoke backend would avoid.

**Why not PCSX2 GS as code:** STUDY as reference, REJECT as import. Licence (GPL-3.0-or-later) is compatible in principle, but architecture is the reason: importing it means importing `common/` threading, shader compilers, texture loaders, and the whole `GSDevice` web; it duplicates every layer Zmbor already owns (frontend, GIF arbiter, VRAM model, presentation) and requires reconciling two GS state machines; it is a conventional render-target renderer with the aliasing-workaround family paraLLEl-GS avoids; and it carries **`GSHwHack.cpp` — per-game rendering hacks — which Zmbor's rules forbid importing regardless of licence**. As reference it remains the best-documented second opinion for open GS behaviour questions (dither DIMX encoding, AA1 fringe/coverage, COLCLAMP wrapping, swizzle) — but a second opinion is not evidence and never upgrades a status to VERIFIED. The only recommended PCSX2 experiment is a documentation sweep recording its behaviour at pinned commit `6a33137aa9ab594e2517eb0bff46b1794b775234` for each open gap-matrix disagreement. "PCSX2 compatibility" as a benefit is explicitly rejected as a Zmbor goal.

**Cheaper parallel tracks (do not block the renderer path, do not displace recompiler work):**
- **SIMDe (PROTOTYPE, narrow, option-gated):** MIT, header-only, cleanest licence story in the category (commit `65c15e6b545361f6dd89b1e3bcc76273e1344653`). Case is testable, not assumed: whether every intrinsic Zmbor emits lowers correctly/efficiently through sse2neon on ARM64 is UNKNOWN; integration is a CMake option `PS2X_USE_SIMDE` (default OFF, exclusive with `USE_SSE2NEON` per TU) under the existing `PS2_*` macro seam. Gate (E4): differential bit-identical integer / documented float-equivalence, suite parity, compile-time regression < ~25%, plus either a demonstrated sse2neon hole or an ARM speed win — else dropped to DEFER. It does not fix the VU scheduler bottleneck or CPU-GS speed.
- **Threaded CPU rasteriser + presentation mailbox (DEFER implementation, approve E3 experiment):** Zmbor-owned GPL-3.0, `std::thread` only — the only threading candidate that does not raise the platform floor (works everywhere, including no-Vulkan hosts). Two separable mechanisms: tile/scanline-parallel rasterisation with provably disjoint pixel ownership, join before `Flush`/`Sync`/`Present` returns; and a bounded presentation mailbox (drop, never tear, never block the game thread on present). Gate: tile-parallel beats serial on textured scenes without differential mismatch and without small-scene regression. A negative E3 is a complete answer for this and for enkiTS (DEFER: no job-parallel workload exists in Zmbor today; do not buy a scheduler for jobs that do not exist).
- **Granite (PROTOTYPE only inside the adapter):** MIT (`69f450c…` HEAD verified) — but that HEAD is **NOT** the Granite commit pinned by parallel-gs `cc6184a`'s submodule pointer, which was not verified (shallow clone left the submodule empty). Do not assume HEAD Granite works with the pinned parallel-gs. Its sole Zmbor value is giving paraLLEl-GS a `Vulkan::Device`; adopting its full renderer would be scope creep, and two Vulkan device owners in one process (Granite + a later bespoke device, or Granite + raylib GL) is a design problem to settle before adapter work, not during it.

### 4.2 Suggested staging for the implementation milestone

- **Stage 0 — seam + contract (CPU-only, mandatory first).** Land Seam B (§3.2) + the §3.1 contract documentation (ordering contract, error channel, swap-state rule) against the CPU backend only. Acceptance: `ps2x_tests` + CTest + IOP green in both modes; gs_sprite golden output byte-identical with the seam present-but-disabled; null raw-GIF backend (records packets, changes nothing) also byte-identical; backend-replacement test extended to CLUT/cache state. If any of these fail, stop.
- **Stage 1 — harness + fixtures (E1 complete).** Generalise the offline harness (§3.3) + the full fixture set (F-flat through F-large) + CPU golden outputs + pre-registered tolerances. Acceptance: CPU backend reproduces all golden outputs; fixture timings (median of ≥21 runs, draw vs present separated, guest wait-loop excluded) recorded as the new baseline. This stage needs no GPU code.
- **Stage 2 — adapter (gated).** paraLLEl-GS adapter behind a CMake option (e.g. `PS2X_ENABLE_PARALLEL_GS`, default OFF), `PARALLEL_GS_STANDALONE` mode, platform null; if Vulkan/Granite is unavailable, configure fails clearly and the CPU backend remains default — never a silent fallback that changes output. Runtime selection at startup only, never per-frame, never game-specific. Presentation is GPU→CPU readback + the existing raylib `UploadFrame` (keeps debug UI, frame dumps, `check_frame.py` working); direct GPU presentation is a later, separate decision. Mapping: `RawGif` → `ParallelGS::GSInterface::gif_transfer(path, data, size)`; Zmbor `GSRegisters` → paraLLEl-GS `PrivRegisterState` + two `ContextState`s; privileged writes forwarded via `write_register`; SIGNAL/FINISH/LABEL/CSR stay in the frontend; vsync/scanout → `vsync(VSyncInfo{...})`; readback via `map_vram_read`/`map_vram_write` + page-tracker syncs. Acceptance: the E2 gate (§3.4): E2a flat tolerance-0 + page-hash identical; E2b textured/blended within pre-registered tolerance; E2c feedback page-hash agreement; E2d CRTC field order/no vertical shift; E2e throughput on F-large with CPU/GPU/readback times separated.
- **Stage 3 — only if E2 passes.** Direct GPU presentation decision; platform coverage (Windows + Linux first; macOS/MoltenVK untested for paraLLEl-GS; Android Vulkan untested; Vita has no Vulkan and keeps the CPU backend as default forever); Granite pin + full licence enumeration before any binary ships; two-backend maintenance plan (both backends differentially tested on every GS change, or the Vulkan one rots — say which one the project commits to maintaining).
- **Falsifiers (any one stops or re-scopes the adapter):** E2a failure; systematic whole-format-class failure tracing to the adapter (not to a known paraLLEl-GS limit); feedback failure tracing to the page tracker; E2 mapping failure (not on known paraLLEl-GS limits) — the prototype stops, it does not become a fork; small-scene regression (GPU submission + readback exceeding CPU draw on S0-class scenes) — the Vulkan path stays opt-in and CPU stays default, and the performance case is re-scoped to large scenes only.

### 4.3 What would change this recommendation

- A failed E2 gate for paraLLEl-GS (see falsifiers) → re-open bespoke Vulkan vs improved CPU (D7) with fresh evidence; do not default to bespoke on reputation.
- An LGPL/policy objection to paraLLEl-GS → bespoke Vulkan becomes the live option, with its §4.1 cost accepted explicitly.
- An E3 result showing tile-parallel CPU beating serial on textured scenes without mismatch → the CPU path's ceiling rises and the GPU case narrows to large scenes; the adapter staging does not change, but its priority drops.
- A demonstrated sse2neon hole or ARM speed win (E4) → SIMDe prototype proceeds independently of the renderer path.

### 4.4 Open questions before a FINAL recommendation (from the research program, adopted)

1. E1 fixtures + timings beyond this milestone's flat/primitive scenes (textured/CLUT, blended, feedback, CRTC, large scene) with CPU golden outputs.
2. ADR 0001 seam implemented and proven byte-identical + null-backend proven (§3.2, Stage 0).
3. E2 gate results (§4.2, Stage 2).
4. Granite commit pinned by parallel-gs `cc6184a` (read from a full checkout via `submodule status`, not HEAD `69f450c`); headless `GSInterface::init` on Windows + Linux; full licence enumeration of the pinned Granite tree, plus Vulkan-Headers (provisional Apache-2.0) and VMA (provisional MIT) confirmed from pinned checkouts, before any binary ships.
5. E3/E4 gates for the cheaper candidates.
6. Unresolved semantic mappings: FINISH → front-end CSR update ordering vs paraLLEl-GS's `SignalInterface`; interlaced field blending, FFMD promotion, CRTC offsets as adapter choices needing golden-frame tests per mode; guest VRAM readback synchronisation through the adapter's sync path (VERIFY spot reads); VSync/field timing vs Zmbor's synchronous VU execution and `vsyncTick` scheduler model (flagged, not resolved).
7. Platform/coexistence: Vulkan device coexistence with raylib's GL context in one process; macOS (MoltenVK, untested), Android Vulkan, and Vita (no Vulkan — CPU backend must remain default and maintained) behaviour; minimum-GPU floor for any port enabling the Vulkan path.
8. Scope guardrail: GS backend work is assessment blocker 4, below recompiler correctness (stripped-ELF discovery, indirect control flow, silent syscall surface, COP0). A GPU backend tuned against synthetic sprites before real scenes exist would be tuned against the wrong target; the prototype order (measure first, cheap reversible SIMDe experiment, then the gated renderer) must not displace that work.

## 5. Limitations (unsoftened)

- This is an assessment, not an implementation: no Vulkan renderer, no paraLLEl adapter, no new rasteriser was built; no GPU code was run. All backend-choice language is conditional on Stage 0–2 gates.
- The recommendation is PROVISIONAL for the reasons in §4.1/§4.4. The research program's graphics work is Stage 1 only: no combined landscape/feasibility-matrix/design document exists yet, no graphics POC exists (poc1 = Ghidra function recovery, poc2 = angr/Capstone CFG, poc3 = compile-speed baseline), and no backend was built or run in that pass. This assessment's measurements are the first Zmbor GS timings, and they cover flat/primitive scenes only.
- Audit verdicts rest on code reading + located tests, not on hardware captures: every GS behaviour (register semantics, swizzle, blend/AA1/dither/fog equations, CLUT timing, TEXFLUSH semantics, ZTST/ZTE interaction, scanmask, display timing) is a model choice in-tree, several explicitly PCSX2/DobieStation-derived, and none is hardware-verified in-tree.
- Measurement conditions are VM-specific (AMD EPYC 9D25, 4 vCPUs, Release, no window); absolute numbers will differ on other hosts. The ratios (primitive-heavy scenes ~4–6× the S0 draw cost; present ~2.1 ms floor) are the transferable claims, not the microsecond values.
- The `gs_sprite` golden frame and this milestone's S0 anchor agree exactly, but both exercise the same single configuration; agreement there says nothing about textured/blended/feedback correctness.
- The research program's Zmbor baseline (reference clone `a1bb50b`, 2026-11-03) predates the Phase 15 tree; its GS premise was re-checked against the current tree for this assessment (`gs.vulkan`/`gs.pgs` still MISSING, only `gs_cpu_backend.cpp` implements the backend), but its other category conclusions were not re-verified here.
- No performance measurement of the texture cache, of guest-CPU-vs-GS frame split, or of the raylib window-upload path is included (see §2.5).

## 6. Verification and deliverables

**Verification (actually run for this milestone):**
- `ps2x_tests` Release: **641/641** from `/tmp` (Phase 15 baseline, unchanged — no tree edits to regress).
- `ps2x_tests` ASan+UBSan: **641/641**, zero sanitizer reports (binary count confirms currency: Phase 14 was 633).
- IOP standalone import suite: **passed** (regression confirmation; no IOP code touched).
- Full CTest Release under `xvfb-run -a`: **100% tests passed, 0 failed out of 56** (55 passed / 1 skipped z3 by design / 0 failed); `e2e_gs_sprite` individually: **passed** (4.25 s wall, wait-loop-dominated — not a GS measure, §2.4).
- These suites are regression confirmation runs, NOT proof of new behaviour: this milestone adds no product code and no test-tree code. The measurement harness lives outside the repo tree (`/tmp/gs_bench/`, preserved at `goals/zmbor-ps2-recompilation-engineering/hidden_files/gs_bench.cpp` with its S0 PPM).

**Deliverables:**
- This report: `goals/zmbor-ps2-recompilation-engineering/files/gs-backend-assessment.md` (goal-files document, NOT a repo-tree file — the working tree stays clean for the implementation milestone).
- Ledger section appended to `your_files/zmbor-engineering-ledger.md` (audit-only milestone format). **Ledger incident, disclosed in full:** during delivery the coordinator accidentally deleted the original 3,886-line ledger (it removed what it believed was a stray duplicate, but the file was the ledger itself, reached through the `your_files/` → goal-files symlink). Recovery was attempted first (full-content grep of workspace/memory/tmp found no other copy; the runtime tool-call database holds no ledger writes; no process held the file open; overlayfs offers no undelete) and failed, so the ledger was **reconstructed** (2,091 lines) from MEMORY.md, the 2026-11-03 daily notes, the parent conversation's compaction summary, and the GS section text itself, with a prominent reconstruction header stating that original verbatim wording is lost except where quoted in those sources. All load-bearing facts (baselines, counts, patch names, decisions, limitations) are preserved; if the parent agent holds any fuller copy, it supersedes the reconstruction. The assessment report itself was never at risk (separate file, intact).
- **NO patches.** Per the milestone brief, patches are produced if and only if the milestone changes product code (minimal harness hooks). It did not: the harness is external, the report and ledger are outside the tree, and no hook was needed. Manufacturing a patch from documentation would violate the brief. The implementation milestone (Stage 0) is where the first patch belongs.
- Working tree after this milestone: still 50 files vs baseline (verified at delivery time); remote untouched; `components.json` statuses untouched.
