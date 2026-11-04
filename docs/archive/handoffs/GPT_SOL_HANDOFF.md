> [!WARNING]
> Archived historical session document (original path: `GPT_SOL_HANDOFF.md`).
> References to local processes, paths, Git branches and test results are historical.
> For current status see [the README](../../../README.md).

# Zmbor — engineering handoff to GPT-Sol

Newest performance checkpoint: implementation HEAD `f02d674aede120b81383e6ee1a51298b96edd6e9`.
`build-ff-fast` is a fourth tree: same RelWithDebInfo/FFmpeg/Od settings as build-ff, aggressive logs OFF.
Unit 697/697. Sequential 35-second neutral-input runs: early VSync progress 174 -> 330, peak memory 270 -> 212 MB;
not gameplay FPS. New configurations now default aggressive logs OFF. Do not re-enable them for a normal play run.
The followed visible process is `sol_visible_fast1`, PID2884, normal input, no automatic timeout.
The previous visible abort run ended by user close (exit0), after both Vivendi and TTIDENT and Crash/Cortex title art.
Hardware availability correction: 78 binaries DO exist under `D:/Project/Zmbor/thirdparty-data/ps2autotests/tests`.
IOP real-PS2 comparisons re-run: 23/23 programs, 1306/1306 lines, no regressions. EE55 not yet re-run.
The earlier claim that hardware binaries were absent from this machine is superseded.

Latest live checkpoint: `sceMpegGetPictureAbort` is now implemented and unit-tested (697/697), and its
16-byte SDK leaf is explicitly hash-verified in analysis metadata before generation. The scanner's 256-bit
conservative threshold remains unchanged. `sol_visible_abort2` is running visibly with normal input and no
automatic timeout at the user's request. Do not launch a competing game or replace its executable while running.
Real-game abort/next-movie progress remains to be checked; full CTest/E2E/IOP most recently passed before abort.
Latest implementation HEAD: `051a034b65c51b04e6fab3218c81b1d7b64c605e` (local, not pushed).
Live PID30132; launcher stays attached until the user closes the game.

Continuation 2026-11-04: see `docs/engineering/CRASH_TWINSANITY_CONTINUATION.md` for newer evidence.
The original handoff below is retained as history. The CD-layer wrapper has now been regenerated and validated:
the game finds VIVENDI.PSS, reads 16 KiB and reaches MPEG demux; the old 0x80000597 bind loop is gone.
Stage 9 remains the highest verified milestone; animated Vivendi intro video now displays through FFmpeg/IPU-DMA HLE.
Latest regression result: FFmpeg-enabled unit 697/697, CTest 49 pass + 1 skip,
E2E 6/6, IOP 4/4. FFmpeg-enabled symbol build completed and ran; the next confirmed blocker was idle-IPU
DMA input ownership under MPEG HLE. See the continuation report for callback/DMA fixes, tests and current run.
Local completed commits after the handoff: `1ac7b7e` CD safety/path lookup, `11430fb` pad playback,
`337b878` bounded IPU HLE input consumption, `5926092` evidence preservation/external profiling.
`71c0ffa` adds MPEG refill/callback lifecycle/DMA bridge, real codec fixture and bulk-input span safety.
Implementation HEAD at that checkpoint: `71c0ffa0dbc1fe1389ea2c5d41647723fd5256ba`.
The first movie-enabled run crashed later in an unchecked bulk PSS copy; CDB reproduced it, and full-span
validation now rejects invalid input with exact argument diagnostics. The latest run `sol_safe_input1` includes
timed START/CROSS/analog playback. Do not claim gameplay or complete movie EOF handling without its evidence.
`build-ff` is now RelWithDebInfo, FFmpeg ON, game /Od. Do not treat it as the old Release/O2 configuration.

Prepared 2026-11-04 by Claude (Opus) at the end of three Crash Twinsanity bring-up sessions. Everything below was
measured on the machine described in §2 unless marked otherwise. Evidence classes used throughout:

- **VERIFIED** — reproduced by a run or test whose output is cited.
- **HYPOTHESIS** — supported by evidence but not proven.
- **INCOMPLETE** — started, not finished; the exact stopping point is given.

The detailed reports (Arabic) are `docs/engineering/CRASH_TWINSANITY_MASTER_REPORT.md` and
`docs/engineering/CRASH_TWINSANITY_BLACK_SCREEN.md`. This file is the operational source of truth.

---

## 1. Repository state

| item | value |
|---|---|
| Checkout | `D:\Project\Testing` (git repo; `games/` and `work/` inside it are git-ignored) |
| Remote | `origin = https://github.com/m7kor/Zmbor.git` |
| Branch | `engineering/crash-twinsanity-full-system` (**local only — not on GitHub**, no PR) |
| Base | `origin/main` = `0ec3f4eb63df37e6b0b2bbf67420bc8e432e186a` (merge of PR #12) |
| HEAD before this handoff commit | `642a11e41a2db9fa2040d4b7a9f2c83332c79c56` |
| Uncommitted changes at handoff | none (this file, the resume prompt and `tools/game_pipeline/` are committed with the handoff) |
| Commit identity used | `git -c user.name=m7kor -c user.email=alshariefk@gmail.com commit ...` with the trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` |

Local commits not on GitHub (oldest first):

| commit | subject | status |
|---|---|---|
| `af499da` | Fix Windows MSVC build (rabbitizer `CONST` vs `<windows.h>`) and the vf0 generated-code test under MSVC; ignore `games/`, `work/`, `*.iso` | VERIFIED |
| `f6537ba` | ElfParser: recover vtable accessor methods beyond the 8-word leaf window (`0x25a4e0`) | VERIFIED, mutation-checked |
| `210e363` | Runtime CMake: `PS2X_GAME_GENERATED_DIR` → `ps2x_game` target (out-of-tree generated code) | VERIFIED |
| `9b01f81` | Docs: session-1 report | — |
| `116554f` | **IOP: IOP-as-client RPC to EE servers (sifcmd 15/16/18)** — root cause of the black screen | VERIFIED, mutation-checked |
| `367db06` | ElfParser: non-branch filler between a return and a vtable accessor (`0x25ae88`) | VERIFIED, mutation-checked |
| `98be1cf` | Runtime: truthful exit status (`GuestTermination`), bounded diagnostics (`ZMBOR_TRACE_*`), `ZMBOR_RUN_SECONDS`, frame series | VERIFIED (e2e `missing_target`) |
| `4dc1dc1` | Trace core split (fixes GS-cache test link) | VERIFIED |
| `25429d2` | Docs: session-2 reports | — |
| `5e99f8a` | CMake: FFmpeg prebuilt pinned by SHA-256, `PS2X_FFMPEG_ARCHIVE` local archive | VERIFIED (configure); full build INCOMPLETE |
| `642a11e` | HLE: `sceCdLayerSearchFile` | unit test VERIFIED, mutation-checked; **in-game effect NOT TESTED** |
| (next) | Handoff docs + `tools/game_pipeline/` | — |

Rules inherited from the project (keep them): never push or merge without the owner's explicit OK; never force-push;
never commit game data, BIOS, or generated game C++; never edit hardware `.expected` files; every behaviour fix
gets a regression test that fails with the fix reverted; report only measured results.

## 2. Machine and toolchain

Windows 11 IoT Enterprise LTSC 2024 (10.0.26100) x64; MSVC 14.44.35207 (VS Build Tools 2022 17.14) at
`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`; Windows SDK 10.0.26100; CMake 4.4.3; Ninja;
Python 3.12 (+ Pillow); `gh` CLI authenticated; GPU RTX 3060 Ti (raylib/OpenGL 3.3, hidden window).

**PATH quirk:** the user PATH contains `C:\Program Files\PowerShell\7"` (stray quote), which breaks `vcvars64.bat`.
`work/scripts/msvc.env` holds a captured MSVC environment; `work/scripts/env.ps1` loads it. To regenerate:

```powershell
$env:PATH = (($env:PATH -split ';') | ? { $_ -and $_ -notmatch '"' }) -join ';'
$env:PATH += ';C:\Program Files\PowerShell\7;C:\Program Files (x86)\Microsoft Visual Studio\Installer'
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && set' |
  ? { $_ -match '^(PATH|INCLUDE|LIB|LIBPATH|WindowsSdkDir|VCToolsInstallDir|UCRTVersion|WindowsSDKVersion|VCINSTALLDIR|VSINSTALLDIR|Platform)=' } |
  Set-Content -Encoding utf8 D:\Project\Testing\work\scripts\msvc.env
```

FetchContent dependencies are pre-cloned (shallow) in `work/deps/{elfio,toml11,fmt,libdwarf,rabbitizer,json,raylib,imgui,rlimgui,sse2neon}`
and passed via `FETCHCONTENT_SOURCE_DIR_*` by `configure.ps1` (toml11's default fetch stalls on submodules).

Bash on this host (Git Bash) intermittently fails to fork ("cygheap read copy failed"); retry, or prefer PowerShell.
Long heredocs in Bash may fail to parse — write scripts to a file instead.

## 3. Zmbor architecture (where things are)

```
game ELF ──► ps2xAnalyzer (ps2_analyzer) ──► TOML (stubs, entry_points, mmio, jump_tables)
              ps2xRecomp/src/lib/elf_parser.cpp  function discovery: symbols/DWARF/SCE DB, fallback scans
                                                 (ScanDataFunctionPointerTables, IsStraightLineLeafMethod),
                                                 recursive descent (discoverFromEntryPoint)
          ──► ps2xRecomp (ps2_recomp) ──► C++: one function per guest function + register_functions.cpp
              code_generator.cpp, control_flow_emitter.cpp (inline HLE calls), ps2_recompiler.cpp (stub wrappers,
              emits PS2X_HLE_TRACE)
          ──► ps2x_game = ps2xRuntime/src/main.cpp + generated code + ps2_runtime (ps2xRuntime/CMakeLists.txt)
ps2_runtime: ps2_runtime.cpp (run loop, termination, frame dump/series, summary), Kernel/EeScheduler.cpp
             (threads, VBlank pacing), Kernel/Syscalls/* (RPC.cpp: EE SIF RPC incl. findEeRpcServerForIop /
             queueEeRpcServerCallForIop), Kernel/Stubs/* (HLE: CD.cpp, MemoryCard.cpp, SIF.cpp, MPEG.cpp, Pad...),
             ps2_memory.cpp (DMAC/VIF/GIF), gs/gs_cpu_backend.cpp (software GS), ps2_ipu.cpp, ps2_vfs.cpp,
             ps2_iop_host.cpp (IopHost adapter), zmbor_trace*.cpp (diagnostics)
ps2xIOP:     iop_subsystem.cpp (HLE routes + emulator), emulator/iop_emulator.cpp (R3000A, import dispatch,
             counters), emulator/services/iop_rpc.cpp (SIF RPC both directions), emulator/imports/iop_cdvd.cpp
             (cdvdman + virtual ISO built from the disc directory), core/iop_kernel.cpp (threads/semaphores)
ps2xTest:    src/*.cpp (MiniTest unit, 686 tests), e2e/ (assembled guest ELFs → native, exit-code + frame checks)
```

## 4. Crash Twinsanity pipeline

| item | path / value |
|---|---|
| Game (user-supplied, private) | `games/Crash Twinsanity (USA) (v2.00).iso` (SHA-256 `49f8a5f7…352e`) and extracted `games/Crash Twinsanity (USA) (v2.00)/` |
| Boot ELF | `SLUS_209.09`, SHA-256 `8d114879fb704d328394771aec6276136bdb4ecf4c5f8de06139b7b42974038f`, entry `0x100008`, stripped |
| Analysis copy + TOML | `work/crash_twinsanity/analysis/SLUS_209.09{,.toml}` (TOML `output` rewritten to `work/crash_twinsanity/generated/`) |
| Generated C++ | `work/crash_twinsanity/generated/` (8,696 files, ~144 MB, deterministic). **Generated before `642a11e`** → must be regenerated for the new CD handler |
| Disc run directory | `work/crash_twinsanity/disc/` = copied `SLUS_209.09` + `System.cnf` + directory junctions `CRASH6`, `FMV` → the game folder (runs never write into `games/`; memory card lands in `disc/mc0`) |
| Diagnostics | `work/crash_twinsanity/diagnostics/<tag>.{stdout,stderr,summary.txt,json,tty.log}`, build logs `native_build*.log`; session-1 originals archived in `work/crash_twinsanity/archive/session1/` |
| Screenshots | `work/crash_twinsanity/screenshots/<tag>.frame.png`, `<tag>_series/`, `<tag>_sheet.png` (local only) |
| Scripts | `work/scripts/` (copies in `tools/game_pipeline/`) |

Build trees (all MSVC Release, Ninja, `PS2X_BUILD_STUDIO=OFF`, `PS2X_IOP_BUILD_TESTS=ON`, `PS2X_ENABLE_SCCACHE=OFF`):

| tree | generated code | FFmpeg | state |
|---|---|---|---|
| `build` | `/Od` | OFF | complete; all tests; `ps2x_game.exe` built from the pre-`642a11e` generated code |
| `build-o2` | `/O2` (`PS2X_GAME_OPTIMIZE=ON`) | OFF | complete at `4dc1dc1` sources |
| `build-ff` | `/O2` | ON (`PS2X_FFMPEG_ARCHIVE=work/deps/ffmpeg-n7.1-241205/ffmpeg-n7.1-241205-lgpl-amd64-shared.zip`) | **configured; build interrupted** (no `ps2x_game.exe`). Consider reconfiguring with `-DPS2X_GAME_OPTIMIZE=OFF` for faster iteration (161 s vs ~1,140 s game compile) |

## 5. Commands

```powershell
# environment for every MSVC command
. D:\Project\Testing\work\scripts\env.ps1

# configure (examples)
& work\scripts\configure.ps1 -Dir build -Extra @('-DPS2X_GAME_GENERATED_DIR=D:/Project/Testing/work/crash_twinsanity/generated')
& work\scripts\configure.ps1 -Dir build-ff -Extra @('-DPS2X_ENABLE_FFMPEG=ON','-DPS2X_FFMPEG_ARCHIVE=D:/Project/Testing/work/deps/ffmpeg-n7.1-241205/ffmpeg-n7.1-241205-lgpl-amd64-shared.zip','-DPS2X_GAME_GENERATED_DIR=D:/Project/Testing/work/crash_twinsanity/generated','-DPS2X_GAME_OPTIMIZE=OFF')

# build tools/tests only (fast), or everything
cmake --build build --parallel 10 --target ps2x_tests ps2_analyzer ps2_recomp
cmake --build build --parallel 10

# regression gates (run e2e serially and without another heavy build running: parallel GL windows under full
# CPU load time out inside raylib init)
$env:PS2X_HIDDEN_WINDOW='1'
Start-Process .\build\ps2xTest\ps2x_tests.exe -RedirectStandardOutput work\unit.log -NoNewWindow -Wait -PassThru  # exit code = failures
ctest --test-dir build\ps2xTest -LE "hwtest|e2e" -j 6
ctest --test-dir build\ps2xTest -L e2e -j 1 --output-on-failure
ctest --test-dir build\ps2xIOP -j 4

# full game pipeline: re-analyze + recompile + build + bounded run (~4-5 min with /Od)
& work\scripts\crash_pipeline.ps1 -RunSeconds 90 -Tag mytag -Trace all -SeriesMs 3000
# reuse generated code and the existing exe
& work\scripts\crash_pipeline.ps1 -SkipRecompile -SkipBuild -BuildDir build-o2 -RunSeconds 120 -Tag t2 -SeriesMs 2000
python -I work\scripts\contact_sheet.py work\crash_twinsanity\screenshots\t2_series work\crash_twinsanity\screenshots\t2_sheet.png 8
```

Runtime environment variables (all opt-in): `PS2X_HIDDEN_WINDOW=1`, `PS2X_DUMP_FRAME=<ppm>`, `PS2X_TTY_LOG=<file>`,
`ZMBOR_RUN_SECONDS=N` (self-stop → exit 124), `ZMBOR_SUMMARY_FILE=<file>`, `ZMBOR_TRACE_{HLE,SYSCALL,IO,GS,ALL}=1`,
`ZMBOR_TRACE_EVENTS=N`, `ZMBOR_FRAME_SERIES=<dir>`, `ZMBOR_FRAME_SERIES_MS=N`.
Exit codes: 0 guest exit/user close, 3 unresolved control flow, 4 guest fatal, 5 unsupported, 6 host failure,
124 harness timeout. The summary contains GS display registers, DMA/GIF/VIF/VSync counters, HLE/syscall counters
(first-seen order, v0 histogram), I/O audit, GS draw targets, DMA channel starts, and IOP counters (`iop import
lib:ord:hle|guest|missing`, `rpc_server`, `rpc_call`, `iop_client`, `cd_search`, `cd_read`).

Profiling: no function-level profile exists yet. `tools/bench/sampling_profiler.*` profiles only its own thread
in-process and needs `/Zi` + `/DEBUG`. Suggested: build a `RelWithDebInfo` tree (or add `/Zi` to `ps2x_game` and
`ps2_runtime`) and use Windows Performance Recorder (`wpr -start CPU` / `wpr -stop out.etl`) or the VS profiler on a
90 s `-SkipRecompile -SkipBuild` run.

## 6. Results so far

### Milestones (Crash Twinsanity)
| stage | status | evidence |
|---|---|---|
| 1–7 identify, parse, discover (8,701 functions), recompile (0 unhandled instructions), compile, link, runtime init | VERIFIED | session-1/2 reports |
| 8 guest program executes | VERIFIED | 90–150 s runs, 8 real IRX modules loaded on the emulated IOP |
| 9 visible startup | **VERIFIED** | frame series: logo + legal screen (≈1–16 s), "Checking memory card (PS2)…" (≈20–53 s), fade to black (≈56–59 s) — `series1_sheet.png`, `perf_od_sheet.png`, `perf_o2_sheet.png` |
| 10 interactive gameplay | NOT REACHED | see §7 |

### Tests at HEAD `642a11e` (MSVC 14.44 Release)
`ps2x_tests` 686/686 (after `642a11e`); at `4dc1dc1`: ctest non-e2e 49 pass + 1 skip (`z3_int_semantics`, z3 not
installed), e2e 6/6 (`-j1`), `ps2xIOP` ctest 4/4. ctest/e2e/IOP were **not re-run after `5e99f8a`/`642a11e`**
(only `ps2x_tests`); re-run them first. Hardware tests (ps2autotests): BLOCKED, the ELFs are not on this machine.
GitHub CI for this branch: never run (not pushed). `main`'s CI had `windows-msvc-x86_64` failing (fixed by `af499da`, unverified on CI).

### Performance (VERIFIED, 90 s, no tracing, same sources `4dc1dc1`)
| | `/Od` | `/O2` |
|---|---|---|
| guest VSyncs | 1,109 (12.1/s) | 1,430 (15.3/s) |
| time to memory-card screen | 22.4 s | 19.6 s |
| CPU / peak RAM | 113 s / 209 MB | 119 s / 181 MB |
| exe / game build | 224 MB / 161 s | 57 MB / 1,137 s |
Guest output identical (DMA 1,621 / GIF 325 / VIF 6, same frame hashes). VBlank fires only when **both** the guest
cycle count and the 16.667 ms wall deadline are reached (`EeScheduler::processDueDeadlines`), so ~25 % of real
speed means guest-cycle production limits it. HYPOTHESIS: most cost is outside generated code (IOP emulation, GS
software rasterizer, runtime helpers) — unprofiled.

## 7. Active investigation — after the memory-card screen

VERIFIED chain (trace runs `post1`, `mpeg1`, `/O2`, FFmpeg OFF):
1. ≈53–56 s: `sceMcGetDir` (save lookup); screen fades out.
2. ≈56–77/87 s: no new GIF/DMA, no disc reads (counters frozen at DMA 1,621 / GIF 325 / IOP cd reads 239).
3. ≈77.7 s (`/Od`) / 87.1 s (contended `/O2` run): `sceGsSyncVCallback`, `sceIpuInit`, `sceMpegInit`,
   `sceCdBreak`, `sceCdDiskReady`, syscall 0x45, `sceCdSync`, `sceSifInitRpc` (ra `0x2c37f4`), then
   **`sceSifBindRpc(cd=0x3c7340, sid=0x80000597)` from ra `0x2c384c` retried ~every 150 ms** until the run ended
   (279 total binds). `0x2C3778` = guest `sceCdLayerSearchFile` (libcdvd), which had no runtime handler; sid
   `0x80000597` is the ROM cdvdfsv search-file server, which no IOP HLE route or loaded IRX provides
   (`ps2xIOP/src/modules` has only libsd, mcserv, dbcman).
4. No `.PSS` file was ever looked up, so FFmpeg was **not** the blocker at this point.

Fix committed (`642a11e`): EE HLE `ps2_stubs::sceCdLayerSearchFile` (layer 0 → `sceCdSearchFile`, which also
registers the pseudo-LBN map later stream reads use; layer ≠ 0 → 0). **Next step: regenerate** (`crash_pipeline.ps1`
without `-SkipRecompile`) and confirm in `generated/sub_002C3778_0x2c3778.cpp` that the wrapper now calls
`ps2_stubs::sceCdLayerSearchFile` (the analyzer/recompiler bind stubs by runtime handler name), then run with
`-Trace all` and check: `hle:sceCdLayerSearchFile` result, the `.PSS` path requested (HLE detail a1 / stderr
`[sceCdSearchFile]` log), `hle:sceCdStStart/StRead`, `hle:sceMpeg*`, `sceIpu*`, IPU DMA (`dma_start ch=0x1000b0xx`),
and GS uploads of decoded frames.

Then with FFmpeg: build `build-ff`, run the same, and compare frames. HYPOTHESIS: the intro movies (VIVENDI.PSS /
TTIDENT.PSS per the disc) play via `sceMpeg*` HLE (`Kernel/Stubs/MPEG.cpp`, FFmpeg-backed when ON) fed by
`sceCdSt*` streaming; without FFmpeg the frames are stubs (black). UNVERIFIED whether the MPEG HLE path matches
how this game drives libmpeg (it calls guest `_sceMpeg*` internals too — see the TOML `entry_points` list).

Other open items:
- `sceMcGetInfo` always returns 0; ps2sdk `libmc.h` documents -1 for the first call after insertion. Tried in
  session 1 (no visible effect then); not changed (unit test `sceMcGetInfo reports formatted and unformatted states`
  pins 0). Revisit if the game behaves oddly around saves.
- Input: `scePadRead`/`scePadGetState` are HLE; no input has been injected yet. Stage 10 will need a scripted input
  path (none exists in the runtime yet) to press START on the title/menu.
- Possible further undiscovered functions: new `JALR` missing targets are reported as exit 3 with the target address;
  the pattern so far was vtable accessors (fix generically in `elf_parser.cpp`, add a synthetic-ELF test in
  `ps2xTest/src/ps2_recompiler_tests.cpp` near "vtable accessor").
- Performance: profile before optimizing (§5).

## 8. Findings superseded (do not repeat)
- Session 1 claimed "the game never reads CRASH.BD" — wrong; it reads via IOP `STREAM.IRX` (`iop cd_read` counters).
- Session 1's `FMV/`-removal A/B only showed no FMV access in the first 45 s.
- `[frame:upload]` counts only the first logged host uploads, not guest frames; use VSync ticks / frame series.
