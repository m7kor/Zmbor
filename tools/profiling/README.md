# Windows process stack sampling

`sample_process` is an external Windows x64 diagnostic sampler. It attaches to an existing process without runtime changes. It suspends each thread briefly, unwinds up to 64 frames through DbgHelp, then resumes it. Use a matching local PDB for useful function names. Embedded MSVC `/Z7` debug information plus linker `/DEBUG` works with this repository's toolchain.

```powershell
. .\work\scripts\env.ps1
cmake -S tools/profiling -B work/profiling-build-z7 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build work/profiling-build-z7
& tools/profiling/selftest.ps1 -Sampler work/profiling-build-z7/sample_process.exe -OutputPrefix work/crash_twinsanity/diagnostics/sampler_selftest_unique

# Attach to a known ps2x_game PID for 60 seconds, every 10 ms, after a 20-second delay.
& work/profiling-build-z7/sample_process.exe 12345 60 work/crash_twinsanity/diagnostics/profile_unique 10 20
```

Choose a fresh output prefix; the sampler refuses to start if any intended output already exists. The sampler creates `.functions.csv` (per-thread exclusive/leaf and inclusive wall samples), `.stacks.csv` (per-thread raw symbolized stacks), `.threads.csv` (independent observed CPU seconds and sample counts), and `.meta.txt` (process CPU seconds, duration, failures and interpretation limits). Inclusive counts deduplicate recursion within one stack. Optimized inline functions and tail calls may not have distinct frames. Unresolved symbols retain a module plus offset.

**These are wall-time stack snapshots, not CPU-cost samples.** Idle worker threads appear in waiting functions. Use `.threads.csv` to distinguish CPU-consuming threads, and inspect their wait stacks separately. Do not convert wall samples to CPU seconds or sum inclusive counts as disjoint costs. The sampler adds suspension/unwind overhead; benchmark performance with separate unprofiled runs. No guest cycle, VSync, presentation-rate or gameplay-speed metric is inferred from host stacks.

For CPU-time-weighted inclusive/exclusive attribution, prefer WPR's CPU sampling profile and WPA with matching PDBs:

```powershell
wpr -status
wpr -start CPU -filemode
# Run the bounded target. Do not start a second recording over an existing recording.
wpr -stop work/crash_twinsanity/diagnostics/cpu_unique.etl
```

On this session's restricted execution environment, `wpr -start CPU -filemode` failed with `0xc5585011` (`Failed to enable the policy to profile system performance`). No CPU hotspot ranking has been inferred from that failed recording. Existing Release game executables had no matching PDB; use a symbol-enabled build before collecting useful game function names.
