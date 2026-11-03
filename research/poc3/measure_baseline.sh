#!/usr/bin/env bash
# POC 3 baseline measurement for Zmbor Category E (codegen/performance).
# ISOLATED: builds ONLY in ~/workspace/zmbor-research/main-build from the
# READ-ONLY reference clone ~/workspace/zmbor-research/main. Never touches
# ~/workspace/zmbor-work/Zmbor, ~/workspace/zmbor-build*, or any remote repo.
# Usage: ./measure_baseline.sh [configure|build|incremental|sizes|e2e|gen|all]
# Results/logs land in ~/workspace/zmbor-research/poc3/results/.
set -u
MAIN="$HOME/workspace/zmbor-research/main"
BUILD="$HOME/workspace/zmbor-research/main-build"
OUT="$HOME/workspace/zmbor-research/poc3/results"
DEPS="$HOME/workspace/zmbor-deps"
mkdir -p "$OUT"
export FETCHCONTENT_SOURCE_DIR_ELFIO="$DEPS/elfio"
export FETCHCONTENT_SOURCE_DIR_TOML11="$DEPS/toml11"
export FETCHCONTENT_SOURCE_DIR_FMT="$DEPS/fmt"
export FETCHCONTENT_SOURCE_DIR_LIBDWARF="$DEPS/libdwarf"
export FETCHCONTENT_SOURCE_DIR_RABBITIZER="$DEPS/rabbitizer"
export FETCHCONTENT_SOURCE_DIR_RAYLIB="$DEPS/raylib"
export FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$DEPS/nlohmann_json"
# Note: FetchContent override env vars are FETCHCONTENT_SOURCE_DIR_<UPPERCASE_NAME>.
# nlohmann_json's declared name is checked at configure time; if the override
# is ignored, configure still succeeds via the cache (deps are also the
# versions pinned in CMakeLists) — the log records which source dirs were used.
CONF_FLAGS=(
  -S "$MAIN" -B "$BUILD" -G "Unix Makefiles"
  -DCMAKE_BUILD_TYPE=Release
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_DEBUG_UI=OFF
  -DFETCHCONTENT_SOURCE_DIR_ELFIO="$DEPS/elfio"
  -DFETCHCONTENT_SOURCE_DIR_TOML11="$DEPS/toml11"
  -DFETCHCONTENT_SOURCE_DIR_FMT="$DEPS/fmt"
  -DFETCHCONTENT_SOURCE_DIR_LIBDWARF="$DEPS/libdwarf"
  -DFETCHCONTENT_SOURCE_DIR_RABBITIZER="$DEPS/rabbitizer"
  -DFETCHCONTENT_SOURCE_DIR_RAYLIB="$DEPS/raylib"
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$DEPS/nlohmann_json"
)

# Peak-RSS sampler: every 2 s, sum RSS of all cc1plus/g++/make descendants.
# Approximate (sampling, not cgroup accounting); method is stated in results.
sample_peak() {
  local pid="$1" peak=0 cur
  while kill -0 "$pid" 2>/dev/null; do
    cur=$(ps -eo rss,comm | awk '$2 ~ /^(cc1plus|cc1|g\+\+|gcc|ld|as)$/ {s+=$1} END {print s+0}')
    [ "$cur" -gt "$peak" ] && peak="$cur"
    sleep 2
  done
  echo "$peak"
}

step="${1:-all}"
run_timed() { # $1=logfile $2..=command ; prints wall seconds to stdout+log
  local log="$1"; shift
  local t0 t1
  t0=$(date +%s.%N)
  "$@" >"$log" 2>&1
  local rc=$?
  t1=$(date +%s.%N)
  echo "RC=$rc WALL_S=$(python3 -c "print($t1 - $t0)")"
  echo "RC=$rc WALL_S=$(python3 -c "print($t1 - $t0)")" >>"$log"
  return $rc
}

case "$step" in
  configure)
    echo "=== configure ===" | tee "$OUT/configure.summary.txt"
    run_timed "$OUT/configure.log" cmake "${CONF_FLAGS[@]}" 2>&1 | tee -a "$OUT/configure.summary.txt"
    ;;
  build)
    echo "=== full build (parallel jobs: $(nproc)) ===" | tee "$OUT/build.summary.txt"
    T0=$(date +%s.%N)
    ( cmake --build "$BUILD" --parallel "$(nproc)" >"$OUT/build.log" 2>&1 &
      BPID=$!
      PEAK=$(sample_peak $BPID)
      wait $BPID; RC=$?
      echo "BUILD_RC=$RC SAMPLED_PEAK_COMPILER_RSS_KB=$PEAK" | tee -a "$OUT/build.summary.txt"
    )
    T1=$(date +%s.%N)
    echo "BUILD_WALL_S=$(python3 -c "print($T1 - $T0)")" | tee -a "$OUT/build.summary.txt"
    ;;
  incremental)
    # NEVER touch the reference clone (read-only). Incremental probe: delete
    # one object file in OUR build dir only, then rebuild that target and time
    # it (recompile of one generated-code-relevant TU + relink).
    echo "=== incremental: delete function_emitter object, rebuild ps2_recomp ===" | tee "$OUT/incremental.summary.txt"
    find "$BUILD" -name 'function_emitter.cpp.o' 2>/dev/null | tee -a "$OUT/incremental.summary.txt"
    find "$BUILD" -name 'function_emitter.cpp.o' -delete 2>/dev/null || true
    run_timed "$OUT/incremental.log" cmake --build "$BUILD" --parallel "$(nproc)" --target ps2_recomp 2>&1 | tee -a "$OUT/incremental.summary.txt"
    echo "=== incremental: no-op rebuild (nothing changed) ===" | tee -a "$OUT/incremental.summary.txt"
    run_timed "$OUT/incremental-noop.log" cmake --build "$BUILD" --parallel "$(nproc)" 2>&1 | tee -a "$OUT/incremental.summary.txt"
    ;;
  sizes)
    {
      echo "=== binary sizes (bytes) ==="
      find "$BUILD" -maxdepth 4 \( -name 'ps2x_tests' -o -name 'ps2x_e2e_*' -o -name 'ps2_recomp' -o -name 'ps2x_bench_*' \) -type f -exec ls -l {} \; 2>/dev/null
      echo "--- generated e2e sources ---"
      find "$BUILD" -name 'ps2_recompiled_functions.cpp' -exec ls -l {} \; 2>/dev/null
      echo "--- build dir total ---"; du -sh "$BUILD" 2>/dev/null
    } | tee "$OUT/sizes.txt"
    ;;
  e2e)
    {
      echo "=== ctest: e2e label ==="
      ( cd "$BUILD" && xvfb-run -a ctest -L e2e --output-on-failure 2>&1 )
      echo "RC=$?"
      echo "=== per-binary direct runs (timed, headless) ==="
      for b in basic branchdelay vu_macro gs_sprite; do
        bin="$BUILD/ps2xTest/e2e/$b/ps2x_e2e_$b"
        # actual binary path is under build/ps2xTest/...; locate it:
        bin=$(find "$BUILD" -name "ps2x_e2e_$b" -type f 2>/dev/null | head -1)
        elf=$(find "$BUILD" -name "$b.elf" 2>/dev/null | head -1)
        echo "--- $b: bin=$bin elf=$elf"
        if [ -n "$bin" ] && [ -n "$elf" ]; then
          # /usr/bin/time is NOT installed here; use date timing (wall only).
          t0=$(date +%s.%N); xvfb-run -a "$bin" "$elf" >"$OUT/e2e-$b.stdout.txt" 2>"$OUT/e2e-$b.stderr.txt"; rc=$?; t1=$(date +%s.%N)
          echo "rc=$rc wall_s=$(python3 -c "print($t1 - $t0)") stdout:"; cat "$OUT/e2e-$b.stdout.txt"
        fi
      done
    } | tee "$OUT/e2e.log"
    ;;
  gen)
    {
      echo "=== ps2_recomp generation time (re-run on e2e tomls into scratch dirs) ==="
      recomp=$(find "$BUILD" -name 'ps2_recomp' -type f 2>/dev/null | head -1)
      echo "ps2_recomp=$recomp"
      for b in basic branchdelay vu_macro gs_sprite; do
        toml=$(find "$BUILD" -name "$b.toml" 2>/dev/null | head -1)
        echo "--- $b toml=$toml"
        if [ -n "$recomp" ] && [ -n "$toml" ]; then
          d="$OUT/gen-scratch-$b"; rm -rf "$d"; mkdir -p "$d"; cp "$toml" "$d/"
          # The toml's outputPath points at the build tree; generation time is
          # measured by running ps2_recomp verbatim and timing it, then
          # restoring: instead time it into the build dir (idempotent output).
          t0=$(date +%s.%N); "$recomp" "$toml" >"$d/ps2_recomp.log" 2>&1; rc=$?; t1=$(date +%s.%N)
          echo "rc=$rc wall_s=$(python3 -c "print($t1 - $t0)")"
          tail -3 "$d/ps2_recomp.log"
        fi
      done
    } | tee "$OUT/gen.log"
    ;;
  all)
    echo "Run steps in order: configure, build, incremental, sizes, e2e, gen."
    echo "This script intentionally does NOT chain them, so a failure in one"
    echo "step is recorded exactly instead of masked by set -e."
    ;;
esac
