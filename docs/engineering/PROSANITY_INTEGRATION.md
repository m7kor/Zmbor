# Prosanity → Zmbor integration (2026-11-04)

## Scope and provenance

This integration imports **67 game-independent files** from `m7kor/prosanity` at source commit
[`798d80701a977cc0dcecef373d39d145a84f9525`](https://github.com/m7kor/prosanity/commit/798d80701a977cc0dcecef373d39d145a84f9525)
onto Zmbor's base `0ec3f4eb63df37e6b0b2bbf67420bc8e432e186a`.

At the initial snapshot import, all 67 transferred Git blobs matched the Prosanity source.
The subsequent independent review includes Zmbor-specific hardening (RPC whole-range
validation with a RAM-boundary regression test) and standalone documentation repairs;
these reviewed files therefore no longer have byte-identical source blobs.
Zmbor remains the canonical, game-independent recompilation framework. Existing GPL-3.0
and upstream attributions remain intact.

## Imported engineering

- **IOP / SIF RPC / CDVD:** EE↔IOP server communication, import dispatch, CD/DVD and kernel fixes, with tests.
- **Recompiler:** ELF function discovery, generated-code diagnostics, and companion regression tests.
- **Runtime HLE:** guarded CD file lookup, MPEG/IPU state and FFmpeg-backed decoding, PAL/NTSC/GS-related diagnostics,
  guest return and fatal status reporting, memory handling and input playback.
- **Observability:** bounded tracing, frame-series diagnostics, opt-in PAD script replay, performance sampling tools.
- **Build and tests:** related CMake changes, test fixtures and unit/E2E regressions, generic analysis utilities.
- **Status inventory:** synchronized metadata and generated project diagram.

## Intentional exclusions (10 files)

- `README.md`: source-repository rebranding, not a Zmbor runtime improvement.
- `GPT_SOL_HANDOFF.md`, `GPT_SOL_RESUME_PROMPT.md`: personal workstation/session handoffs.
- `docs/engineering/CRASH_TWINSANITY_{BLACK_SCREEN,CONTINUATION,MASTER_REPORT}.md`:
  game-specific evidence, not generalized Zmbor capabilities.
- `tools/game_pipeline/{README.md,configure.ps1,crash_pipeline.ps1,env.ps1}`:
  paths and orchestration specific to a user's Crash Twinsanity Windows checkout.
  Generic PAD playback, ELF SDK-leaf binding, frame image processing and profiler utilities **were** imported.

These exclusions are not claims that Crash Twinsanity works fully, or that any new game has passed a playthrough.

## Verification policy

1. Run CI on **Zmbor's integration branch** before merging (Windows MSVC, Linux GCC and Clang,
   Clang ASan/UBSan, fuzz smoke, inventory/dashboard).
2. Run EE/IOP hardware-grounded tests with the original `.expected` fixtures; never adjust them to fit code.
3. Re-run representative game pipelines (including previous RE4 checks) against user-provided game data
   without committing the data or generated game code.
4. Preserve opt-in diagnostics; avoid claiming performance gain without controlled identical-workload runs.
5. Review `PS2X_ENABLE_FFMPEG` and pinned Windows archive when packaging/distributing;
   the optional accelerated/FFmpeg paths are not proof of universal game compatibility.

Source repository GitHub Actions for `798d807` passed six jobs:
https://github.com/m7kor/prosanity/actions/runs/41843554878 .
This is source CI evidence, **not** a substitute for testing the Zmbor import.

Only this integration branch is modified until its own checks are reviewed.

## Independent review fixes

- EE RPC server descriptors must fit completely inside guest EE memory; an individual mapped first or last byte is not proof of a valid structure.
- IOP-to-EE RPC send buffers are validated as one contiguous RAM/scratchpad span **before** any bulk host `memcpy`; near-end RAM and KSEG1 alias behavior have a new regression test.
- EE RPC replies stop at the real mapped memory boundary rather than wrapping a guest address back into low RAM.
- Generic PAD playback, profiler documentation and tracing references no longer rely on excluded local Crash Twinsanity scripts/reports.
- Re-run the Zmbor CI on the reviewed commit. This validation does not replace EE/IOP hardware corpus or full-game RE4/Crash regression measurements.
