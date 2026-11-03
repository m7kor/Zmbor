# POC 3 — Baseline measurement (Zmbor Category E: code generation & performance)

Isolated POC directory. Nothing here edits the reference clone
`~/workspace/zmbor-research/main` (read-only reference, HEAD
`a1bb50be3c797025d96e5c744e615c632b0a8f83`, 2026-11-03) and nothing here
touches `~/workspace/zmbor-work/Zmbor`, `~/workspace/zmbor-build*`, or any
remote repository. All builds live in this directory tree or in
`~/workspace/zmbor-research/main-build`.

## Environment (as measured 2026-11-03)

- Ubuntu 24.04, GCC/G++ 13.3.0, CMake 3.28.3, GNU Make ("Unix Makefiles";
  Ninja was NOT installed), Python 3, xvfb-run present.
- 4 CPUs, 7.7 GiB RAM. X11/GL dev headers present (`libxrandr-dev`,
  `libxinerama-dev`, `libxcursor-dev`, `libxi-dev`, `libgl1-mesa-dev`).
- NOT installed at start: Clang, Ninja, sccache, `/usr/bin/time`, `bc`.
  `ccache` 4.9.1 was installed via apt DURING this POC (only to run the
  cache experiment); it is a system tool install, not a Zmbor change.
- Load caveat (applies to EVERY wall time below): a separate Phase 15
  coordinator build/test workload shared these 4 cores for much of POC 3.
  Wall times are "as measured under shared load", not isolated-machine
  numbers. Do not quote them as clean-room benchmarks.

## Reproduce the baseline

```bash
# 1. Configure (dependency cache reused READ-ONLY via overrides)
cd ~/workspace/zmbor-research/poc3
./measure_baseline.sh configure   # log: results/configure.log

# 2. Full build (parallel = nproc)
./measure_baseline.sh build       # log: results/build.log, summary: results/build.summary.txt

# 3. Incremental probes (build dir ONLY; the reference clone is never touched)
./measure_baseline.sh incremental # logs: results/incremental*.log

# 4. Binary sizes + generated-source sizes
./measure_baseline.sh sizes       # results/sizes.txt

# 5. e2e CTests + timed direct runs (headless)
./measure_baseline.sh e2e         # log: results/e2e.log

# 6. ps2_recomp generation time (external wall-clock; ps2_recomp prints no timing)
./measure_baseline.sh gen         # log: results/gen.log
```

Full configure flags (also in `measure_baseline.sh`):

```bash
cmake -S ~/workspace/zmbor-research/main -B ~/workspace/zmbor-research/main-build \
  -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_DEBUG_UI=OFF \
  -DFETCHCONTENT_SOURCE_DIR_ELFIO=~/workspace/zmbor-deps/elfio \
  -DFETCHCONTENT_SOURCE_DIR_TOML11=~/workspace/zmbor-deps/toml11 \
  -DFETCHCONTENT_SOURCE_DIR_FMT=~/workspace/zmbor-deps/fmt \
  -DFETCHCONTENT_SOURCE_DIR_LIBDWARF=~/workspace/zmbor-deps/libdwarf \
  -DFETCHCONTENT_SOURCE_DIR_RABBITIZER=~/workspace/zmbor-deps/rabbitizer \
  -DFETCHCONTENT_SOURCE_DIR_RAYLIB=~/workspace/zmbor-deps/raylib \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=~/workspace/zmbor-deps/nlohmann_json
```

## Further experiments (each in its own directory)

```bash
# ccache cold-vs-warm on target ps2_recomp (isolated cache dir)
./ccache_experiment.sh            # summary: results/ccache-experiment.summary.txt

# IOP standalone suite (NOT part of the top-level build; option defaults OFF)
cmake -S ~/workspace/zmbor-research/main/ps2xIOP -B poc3/iop-build \
  -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DPS2X_IOP_BUILD_TESTS=ON
cmake --build poc3/iop-build --parallel 4
(cd poc3/iop-build && ctest --output-on-failure)

# Generated-TU compile experiment + PCH prototype (no clone changes;
# manual g++ invocations using the production flags from compile_commands.json)
# See results/tu-experiment.summary.txt and pch-test/ (pch.h + build notes in results.md).
```

## What was NOT measured here (blockers, not omissions)

- Peak build memory for THIS build alone: UNKNOWN. `/usr/bin/time` is not
  installed and the sampler in `measure_baseline.sh` summed compiler RSS
  system-wide, which included the concurrent Phase 15 build's compilers.
  Its 2,490,680 KB figure is system-wide and must NOT be quoted as Zmbor's
  peak. Re-run on an idle machine with `/usr/bin/time -v` or cgroup
  accounting for a credible number.
- Clang/ThinLTO: NOT tested — Clang is not installed here. Experiment
  design is in the Category E section file.
- sccache warm-vs-cold: NOT run as a separate measurement — the sccache
  CMake hook already exists in main, and ccache was measured instead as
  the compiler-cache proxy (same concept, different tool). Installing the
  sccache binary is all that is needed to activate the existing hook.
- Full-build ccache/sccache comparison: NOT run (a second full build is
  ~15 min under load); the ps2_recomp-target subset (117 compilations)
  was measured instead and its scope is stated wherever it is cited.
- Any real-game runtime performance: UNKNOWN. The e2e corpus is 4
  micro-programs (5–6 KB ELFs); its run times are dominated by runtime
  startup/frame loop, not guest CPU work, and do not generalize to a game.

## File map

- `measure_baseline.sh` — baseline procedure (configure/build/incremental/sizes/e2e/gen).
- `ccache_experiment.sh` — ccache cold-vs-warm procedure.
- `results/` — raw logs + summaries (`results.md` is the readable digest).
- `iop-build/` — IOP standalone build dir (tests 4/4).
- `ccache-build/` — warm-cache build dir from the ccache experiment.
- `ccache-dir/` — isolated ccache cache (117 entries from the experiment).
- `pch-test/` — PCH prototype (`pch.h`, 129 MB `.gch`, build stderr).
