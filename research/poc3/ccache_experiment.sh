#!/usr/bin/env bash
# POC3 ccache experiment (isolated): cold vs warm build of target ps2_recomp
# in a FRESH build dir, cache in poc3/ccache-dir only. Never touches main clone.
set -u
MAIN="$HOME/workspace/zmbor-research/main"
DEPS="$HOME/workspace/zmbor-deps"
B="$HOME/workspace/zmbor-research/poc3/ccache-build"
export CCACHE_DIR="$HOME/workspace/zmbor-research/poc3/ccache-dir"
export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-}"
run() { # $1=label $2=log
  local label="$1" log="$2"; shift 2
  local t0 t1; t0=$(date +%s.%N)
  "$@" >"$log" 2>&1; local rc=$?
  t1=$(date +%s.%N)
  echo "$label rc=$rc wall_s=$(python3 -c "print($t1-$t0)")"
  ccache -s 2>/dev/null | grep -E 'Hits|Misses|Cache size|Files' || true
}
CONF=( -S "$MAIN" -B "$B" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_DEBUG_UI=OFF
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  -DFETCHCONTENT_SOURCE_DIR_ELFIO="$DEPS/elfio" -DFETCHCONTENT_SOURCE_DIR_TOML11="$DEPS/toml11"
  -DFETCHCONTENT_SOURCE_DIR_FMT="$DEPS/fmt" -DFETCHCONTENT_SOURCE_DIR_LIBDWARF="$DEPS/libdwarf"
  -DFETCHCONTENT_SOURCE_DIR_RABBITIZER="$DEPS/rabbitizer" -DFETCHCONTENT_SOURCE_DIR_RAYLIB="$DEPS/raylib"
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$DEPS/nlohmann_json" )
rm -rf "$B" "$CCACHE_DIR"; mkdir -p "$B"
run "COLD-configure" ~/workspace/zmbor-research/poc3/results/ccache-cold-configure.log cmake "${CONF[@]}"
run "COLD-build-ps2_recomp" ~/workspace/zmbor-research/poc3/results/ccache-cold-build.log cmake --build "$B" --parallel 4 --target ps2_recomp
ccache -s > ~/workspace/zmbor-research/poc3/results/ccache-stats-cold.txt 2>&1
rm -rf "$B"; mkdir -p "$B"
run "WARM-configure" ~/workspace/zmbor-research/poc3/results/ccache-warm-configure.log cmake "${CONF[@]}"
run "WARM-build-ps2_recomp" ~/workspace/zmbor-research/poc3/results/ccache-warm-build.log cmake --build "$B" --parallel 4 --target ps2_recomp
ccache -s > ~/workspace/zmbor-research/poc3/results/ccache-stats-warm.txt 2>&1
