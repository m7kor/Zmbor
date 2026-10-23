# Benchmarks

Measured results only. Each row names the workload, build and host; re-run the commands to compare.

Host: Intel Core i5-10400F, 16 GB RAM, Windows 11 x64. Build: MSVC 14.44 + Ninja, Release
(`PS2X_BUILD_STUDIO=OFF`, `PS2X_ENABLE_FFMPEG=OFF`). Date: 2026-10-23.

## End-to-end pipeline (ps2xTest/e2e)

| Workload | Guest ELF | `ps2_recomp` time | Generated C++ | Native compile + link | Run (3 runs) |
|---|---|---|---|---|---|
| `basic` (loop, recursion, 64-bit branch, LQ, itoa, puts) | 5,668 B | 19 ms | 33,329 B / 4 files | 16.1 s | 1.70 / 1.75 / 1.61 s |
| `gs_sprite` (GS setup, GIF DMA + VIF1 DIRECT, 33.5M-iteration wait loop) | 5,364 B | 13 ms | 13,539 B / 4 files | 16.8 s | 5.16 / 5.23 / 5.61 s |

How measured: `ps2_recomp <program>.toml` wall time; `cmake --build build --target ps2x_e2e_<program>` after
touching the two generated `.cpp` files; `PS2X_HIDDEN_WINDOW=1 ps2x_e2e_<program>.exe <program>.elf` wall time.

Observations (not yet investigated further):
- Native compile time is dominated by the runtime headers each generated translation unit includes; the e2e
  targets do not use the precompiled header `ps2EntryRunner` uses.
- `basic` run time is mostly host start-up (window, audio device, runtime init); the guest work itself is tiny.
- `gs_sprite`'s 33.5M-iteration `addiu`/`bne` loop accounts for roughly 3.5 s, about 100 ns per iteration. Every
  backward branch in generated code calls `runtime->eeCheckpointDue()`; that per-iteration cost is the first
  thing to profile before any EE performance work.

## Unit suites

| Suite | Count | Wall time |
|---|---|---|
| `ps2x_tests` | 486 tests | about 5 s |
| `ps2xIOP` ctest | 4 tests | 0.1 s |
