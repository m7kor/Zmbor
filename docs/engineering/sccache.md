# sccache for Zmbor builds (local compiler cache)

Status: installed and measured 2026-11-03 (Phase 20). This document is the
reproducible procedure; the numbers at the end are measurements from that
run, not projections.

## What it is

sccache is a ccache-like compiler cache: it hashes the preprocessed
compilation (source, flags, compiler version) and replays stored object
files on cache hits. It caches *compilation only*. Linking (including the
LTO links this tree uses in Release), code generation (`ps2_recomp`
runs), and CMake configure time are not cached, so a warm build is never
free — see the measurements.

Cache backend here is **local disk only**. No remote storage (S3/GCS/
Redis/GHA) is configured, and no credentials of any kind are involved.

## The hook (how Zmbor enables it)

- Root `CMakeLists.txt` (after `project(PS2RetroX)`, before any
  `add_subdirectory()`): option `PS2X_ENABLE_SCCACHE`, default ON.
  When ON, `find_program(sccache)` runs; if found, it sets
  `CMAKE_C_COMPILER_LAUNCHER` and `CMAKE_CXX_COMPILER_LAUNCHER` in root
  scope, so every subdirectory target inherits the launcher. If sccache
  is absent from PATH, configure prints
  `sccache not found; continuing without compiler launcher` and the
  build is byte-for-byte the build without it (verified: zero launcher
  references in generated build files).
- History, recorded so it is not re-learned the hard way: the hook
  originally lived in `ps2xRuntime/CMakeLists.txt`. That scoped it to
  ps2xRuntime's subtree only. Measured at Phase 20, before the move,
  only 79 of 291 real compile rules were wrapped (ps2xRuntime plus the
  raylib/glfw dependencies declared there); ps2xRecomp, ps2xIOP,
  ps2xAnalyzer, ps2xTest, libdwarf, and fmt were all uncached. After
  moving the block to the root, 279 of 279 real compile rules are
  wrapped (the 4 remaining unwrapped rules are CMake's internal LTO
  test programs, not project targets). POC 3's note "the sccache hook
  already exists, install the binary" was therefore true but
  incomplete: installing the binary alone would have left ~73% of the
  build uncached.
- Scope limit: the root hook covers builds configured from the root
  `CMakeLists.txt`. The standalone IOP suite configures `ps2xIOP`
  directly, so it does not pick up the launcher; that is a separate
  configure, not a defect in this one. IOP code is unaffected by this
  milestone (no IOP source changed).

## Install (pinned, anonymous, checksum-verified)

Pinned version: **sccache 0.19.0** (Mozilla, Apache-2.0), Linux
x86_64 musl portable binary — no Rust toolchain needed, no account,
no token.

```sh
mkdir -p ~/workspace/zmbor-tools/sccache-v0.19.0
cd ~/workspace/zmbor-tools/sccache-v0.19.0
curl -sL -o sccache.tar.gz \
  https://github.com/mozilla/sccache/releases/download/v0.19.0/sccache-v0.19.0-x86_64-unknown-linux-musl.tar.gz
curl -sL -o sccache.tar.gz.sha256 \
  https://github.com/mozilla/sccache/releases/download/v0.19.0/sccache-v0.19.0-x86_64-unknown-linux-musl.tar.gz.sha256
# Verify: sha256sum output must equal the published value below.
sha256sum sccache.tar.gz
# aa73c7270c997894038c0d8f0d297865f57569e1687883322bc431713d4bb9ddaa73c7270c997894038c0d8f0d297865f57569e1687883322bc431713d4bb9dd
tar xzf sccache.tar.gz --no-same-owner
mv sccache-v0.19.0-x86_64-unknown-linux-musl/sccache ./sccache
chmod +x ./sccache
./sccache --version   # sccache 0.19.0
```

Install location matters: `~/workspace/` survives VM replacements;
`/usr/local/bin`, `/tmp`, and apt-installed packages do not. Never
rely solely on a system path for this binary.

## Use

```sh
export PATH="$HOME/workspace/zmbor-tools/sccache-v0.19.0:$PATH"
export SCCACHE_DIR="$HOME/workspace/zmbor-sccache-cache"   # persistent, explicit
export SCCACHE_CACHE_SIZE="10G"                            # sccache default is 10 GiB
cmake -S <zmbor> -B <build> -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_DEBUG_UI=OFF \
  -DFETCHCONTENT_SOURCE_DIR_ELFIO="$HOME/workspace/zmbor-deps/elfio" \
  -DFETCHCONTENT_SOURCE_DIR_TOML11="$HOME/workspace/zmbor-deps/toml11" \
  -DFETCHCONTENT_SOURCE_DIR_FMT="$HOME/workspace/zmbor-deps/fmt" \
  -DFETCHCONTENT_SOURCE_DIR_LIBDWARF="$HOME/workspace/zmbor-deps/libdwarf" \
  -DFETCHCONTENT_SOURCE_DIR_RABBITIZER="$HOME/workspace/zmbor-deps/rabbitizer" \
  -DFETCHCONTENT_SOURCE_DIR_RAYLIB="$HOME/workspace/zmbor-deps/raylib" \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$HOME/workspace/zmbor-deps/nlohmann_json"
cmake --build <build> --parallel 4
```

Configure prints `Using sccache: <path>` when the launcher is active.
An existing build directory configured *without* sccache on PATH does
not retroactively gain the launcher for already-generated rules; use a
fresh build directory (or reconfigure and confirm the launcher appears
in a target's `build.make`) when enabling it.

## Cache management

```sh
sccache --show-stats    # hits, misses, errors, cache size
sccache --zero-stats    # reset counters (does not clear the cache)
sccache --stop-server   # stop the daemon; it auto-starts on next use
```

- Inspect: `sccache --show-stats` reports `Cache location` and
  `Cache size` / `Max cache size`.
- Clear: stop the server, then delete the cache directory contents
  (`rm -rf "$SCCACHE_DIR"` contents; the directory is recreated on
  next use). There is no in-tool "clear all" for the local backend.
- Disable for one build: `-DPS2X_ENABLE_SCCACHE=OFF`, or simply leave
  sccache off PATH (configure falls back to no launcher).
- Invalidation is automatic and conservative: cache keys cover the
  preprocessed source, flags, and compiler version, so a compiler
  upgrade, a flag change, or a header change misses cleanly rather
  than replaying a stale object. A warm-cache build that produces a
  failing test suite is a caching defect, not a speedup — see the
  correctness check below.

## VM-replacement recovery

VM replacements wipe `/tmp`, apt packages, and anything outside `~`.
If `sccache --version` stops working after a replacement:

1. The binary at `~/workspace/zmbor-tools/sccache-v0.19.0/sccache`
   normally survives (it lives under `~`). Re-export PATH and rebuild.
2. If it is gone, repeat the Install section verbatim (same pinned
   version, same checksum). Do not substitute an unpinned build and
   call it the same setup.
3. The cache at `~/workspace/zmbor-sccache-cache` also survives; if it
   is gone, the next build is simply a cold build (see measurements).

## Measurements (Phase 20, actually run)

Machine: this VM, GCC 13.3.0, CMake 3.28.3, Unix Makefiles,
`--parallel 4`, Release, Studio/FFMPEG/DebugUI OFF, dependency-cache
overrides as above, same build-directory path for cold and warm,
stats zeroed before each build. Build wall times exclude ~8 s of
configure time. Tree state: Phase 19 (Ghidra oracle) plus the hook
move in this milestone.

| Quantity | Cold (empty cache) | Warm (same tree, same path) |
|---|---|---|
| Build wall (`cmake --build`) | **557 s**, RC 0 | **205 s**, RC 0 |
| Compile requests | 278 | 278 |
| Cache hits / misses | 3 / 275 (1.08%) | 278 / 0 (100.00%) |
| Cache errors | 0 | 0 |
| Cache size after build | 56 MiB (sccache) | 56 MiB (sccache) |
| Average compiler / cache-read | 5.059 s / — | — / 0.004 s |

- **Cold → warm: 557 s → 205 s, 2.7×.** The 3 cold hits are small
  intra-build repeats (shared compilations), not pre-seeded cache.
- The remaining 205 s warm is linking (LTO, uncached), code
  generation, and build-system work — the floor sccache cannot go
  below for a full rebuild. Incremental rebuilds of one TU were
  already measured by POC 3 at 53.7 s without sccache; a warm-cache
  incremental rebuild should approach link-only time, but that was
  not separately measured here and is not claimed.
- The cold number is reported standalone. It is *not* comparable to
  POC 3's 874.8 s full build (different tree state, different machine
  load, and a cold cache adds hashing/write overhead rather than
  saving time). No speedup is claimed from the cold figure.
- For reference only, POC 3's ccache proxy (a different tool) measured
  5.9× on the `ps2_recomp` target subset alone (cold 134.0 s vs warm
  22.8 s, ccache 4.9.1). That is POC 3's number for ccache, not a
  sccache number, and not a full-build claim. ccache remains the
  measured alternative if sccache is ever unavailable.

## Correctness check (warm-cache build)

A cache hit that corrupts an artifact is a failure, not a speedup.
From the warm-cache build in this milestone:

- `ps2x_tests` (run from `/tmp`): **650/650**.
- Full CTest under `xvfb-run -a`: **100% passed, 0 failed out of 56**
  (55 passed, 1 skipped: `z3_int_semantics`, solver absent by
  design), including all 5 e2e programs.
- IOP standalone binaries (prebuilt, Phase 19 tree; no IOP code
  changed in this milestone, so these are regression sanity, not a
  sccache-build gate): import passed, emulator smoke passed,
  compatibility 18/18, import-version 6/6.

## Limitations

- Local disk cache only; no shared/remote cache, so a fresh VM or a
  different machine starts cold. Cache size is capped at 10 GiB by
  `SCCACHE_CACHE_SIZE`; eviction is LRU by sccache.
- Measurements are this VM and this tree state; other machines,
  core counts, and compilers will differ. Re-measure, do not quote
  these numbers forward across environments.
- sccache version/extension changes require re-measurement, same as
  any toolchain change.
- Peak build memory was not measured here (POC 3 likewise reports it
  as UNKNOWN in isolation); do not quote any memory figure for this
  milestone.
