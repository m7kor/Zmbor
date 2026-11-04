# Game pipeline scripts (Windows, MSVC)

Copies of the scripts used for the Crash Twinsanity (SLUS_209.09) bring-up. The working copies live in the
git-ignored `work/scripts/`, which is where the paths below point (`D:\Project\Testing` is the checkout). They hold
no game data. See `GPT_SOL_HANDOFF.md` at the repository root for the full procedure.

| script | purpose |
|---|---|
| `env.ps1` | loads `work/scripts/msvc.env` (a dump of `vcvars64.bat`'s PATH/INCLUDE/LIB; regenerate it, see the handoff) and `cd`s to the checkout |
| `configure.ps1 -Dir <build> -Extra @(...)` | CMake + Ninja Release configure with pre-cloned dependencies from `work/deps` |
| `crash_pipeline.ps1` | analyze -> recompile -> build `ps2x_game` -> bounded run; writes `work/crash_twinsanity/diagnostics/<Tag>.*` and `screenshots/<Tag>.*` |
| `ppm2png.py` | frame dump -> PNG + statistics (standard library only) |
| `contact_sheet.py` | frame series -> contact sheet + per-frame statistics (needs Pillow) |

`crash_pipeline.ps1` parameters: `-RunSeconds N` (runtime stops itself, exit 124), `-SkipRecompile`, `-SkipBuild`,
`-Tag name`, `-BuildDir build|build-o2|build-ff`, `-Trace hle,syscall,io,gs|all`, `-SeriesMs N` (frame every N ms),
`-Disc <dir>`.

Use a fresh tag for every run: the pipeline refuses existing evidence paths. Analyzer/recompiler logs and TOML
are tagged, and the legacy `disc/ps2_log.txt` function trace is archived under diagnostics after the run (an older
file is preserved as `<Tag>.previous-function-log.txt`). A failed analyzer stops the pipeline.

Optional external profiling:

```powershell
& tools/game_pipeline/crash_pipeline.ps1 -SkipRecompile -SkipBuild -BuildDir build-ff `
    -RunSeconds 150 -Tag NEW_PROFILE_TAG -Trace all -SeriesMs 3000 `
    -Sampler work/profiling-build-z7/sample_process.exe -ProfileSeconds 60 -ProfileDelaySeconds 5
```

Build the sampler using [tools/profiling](../profiling/README.md), and use a game executable with a matching PDB.
The JSON records its exit code. Its stack samples include waits; benchmark using separate runs without it.
`PS2X_ENABLE_AGRESSIVE_LOGS=ON` also enables per-function tracing and can dominate file I/O; record this setting
when comparing performance. Controller scripting is documented in [PAD_PLAYBACK.md](PAD_PLAYBACK.md).

The Crash pipeline adds a reviewed `sceMpegGetPictureAbort` analysis binding using `bind_sdk_stub.py`.
Its 16-byte SDK 3.x leaf is below the SCE scanner's 256-static-bit threshold. The helper checks the exact ELF
function SHA-1 against the existing SDK signature before touching the config; a different game revision fails
closed. The runtime API contains no title address or work-area offset hook. A wrong-signature negative control
left the configuration hash unchanged; the positive binding regenerated the wrapper correctly.
