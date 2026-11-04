> [!WARNING]
> Archived historical session document (original path: `GPT_SOL_RESUME_PROMPT.md`).
> References to local processes, paths, Git branches and test results are historical.
> For current status see [the README](../../../README.md).

# Resume prompt for GPT-Sol — Zmbor / Crash Twinsanity

Paste everything below the line into a new session opened at `D:\Project\Testing`.

---

You are continuing **Zmbor** (https://github.com/m7kor/Zmbor), a PlayStation 2 static recompiler and runtime
(C++20, CMake, MSVC on Windows). You are the principal engineer. The validation target is **Crash Twinsanity USA
v2.00 (SLUS-20909)**, supplied legally by the owner in `games/` (git-ignored). Do **not** restart: continue from the
exact state recorded in `GPT_SOL_HANDOFF.md` (read it fully first), then
`docs/engineering/CRASH_TWINSANITY_MASTER_REPORT.md` and `docs/engineering/CRASH_TWINSANITY_BLACK_SCREEN.md`.

## Ground rules (non-negotiable)
- Work on branch `engineering/crash-twinsanity-full-system` in `D:\Project\Testing`. It exists only locally. Do not
  reset, rebase, delete, or overwrite it, and do not delete `work/` (generated C++, diagnostics, screenshots, deps).
- Never push, merge, or open a PR without the owner's explicit approval. Never force-push.
- Never commit or upload game files, extracted game data, generated game C++, BIOS, or screenshots of the game.
- Evidence only: classify results as VERIFIED / TESTED / PARTIAL / FAILED / BLOCKED / NOT TESTED. A non-black frame
  is not success unless it is identifiable game content produced by Zmbor itself (never PCSX2).
- Every behaviour fix: root cause first, regression test that fails with the fix reverted (mutation-check it), no
  weakened assertions, no edited `.expected` files, no unconditional-success stubs, no game-specific hacks in the
  core (generic analyzer/runtime fixes; a title-specific workaround only behind an explicit identity, documented).
- Commit locally with `git -c user.name=m7kor -c user.email=alshariefk@gmail.com commit` and the trailer used in
  the existing commits. Communicate with the owner in Arabic (technical identifiers in English).
- Run heavy builds and e2e tests separately: parallel e2e windows under full CPU load time out inside raylib init.

## Where things stand (verified)
- Stage 9 reached: Zmbor renders the game's logo/legal screen and the "Checking memory card (PS2)…" screen, then
  fades to black. Root cause of the old black screen: IOP→EE RPC (sifcmd 15/16/18) was unimplemented — fixed in
  `116554f`. Exit codes are now truthful (3 = unresolved control flow, 124 = harness timeout).
- Next blocker (verified by trace): after `sceMpegInit` the game calls guest `sceCdLayerSearchFile` (`0x2C3778`),
  which bound the unserved IOP cdvdfsv RPC sid `0x80000597` and retried forever. An EE HLE handler was committed in
  `642a11e` (unit-tested, mutation-checked) but **has not been validated in the game**: the generated C++ predates it.
- FFmpeg prebuilt verified and pinned (`5e99f8a`); `build-ff` is configured but its build was interrupted.
- Performance: `/O2` gives +29 % guest VSyncs over `/Od` (≈15 vs 12 per second; real hardware 60). Not profiled yet.

## Your first actions
1. `git status`, `git log --oneline origin/main..HEAD` — expect the commits listed in the handoff and a clean tree.
2. Load the MSVC environment: `. D:\Project\Testing\work\scripts\env.ps1` (regenerate `msvc.env` per handoff §2 if
   needed). Rebuild `build` and re-run all gates (handoff §5); ctest/e2e/IOP were not re-run after `5e99f8a`/`642a11e`.
3. Regenerate the game and validate `642a11e`:
   `& work\scripts\crash_pipeline.ps1 -RunSeconds 150 -Tag cdlayer1 -Trace all -SeriesMs 3000`.
   Confirm `generated/sub_002C3778_0x2c3778.cpp` now calls `ps2_stubs::sceCdLayerSearchFile`; in
   `diagnostics/cdlayer1.summary.txt` find `hle:sceCdLayerSearchFile` (args/result), the movie path, and what
   follows (`sceCdSt*`, `sceMpeg*`, `sceIpu*`, IPU DMA channels `0x1000b000/0x1000b400`, GS uploads).
4. Build `build-ff` (FFmpeg ON; reconfigure with `-DPS2X_GAME_OPTIMIZE=OFF` for faster iteration) and compare the
   same run with frames: do movie frames appear? Establish the full chain disc → `sceCdSt*` → MPEG demux/decoder →
   IPU/CSC → GS upload → display before changing anything in it.
5. Continue toward Stage 10 (title screen → main menu → gameplay): fix each newly exposed blocker generically with
   tests; add a scripted controller-input mechanism to the runtime (e.g. an env-var script of pad states per VSync,
   with a unit test) so START/X can be pressed in automated runs; verify with frame series.
6. Profile before optimizing (WPR or VS profiler on a `/Zi` build of `ps2x_game` + `ps2_runtime`); optimize only
   measured hot spots, keep guest-visible results identical (compare DMA/GIF counters and frame hashes).
7. Keep `GPT_SOL_HANDOFF.md` and the Arabic reports updated at each checkpoint; commit tested work locally.

Deliver at the end: an Arabic engineering report (highest verified milestone with screenshots kept local, exact
test counts, benchmark comparisons, causes vs hypotheses, remaining blockers), and the git state.
