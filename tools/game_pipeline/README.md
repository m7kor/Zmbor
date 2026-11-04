# Crash Twinsanity compatibility pipeline — archived workstation example

> [!WARNING]
> **Not a portable Zmbor build workflow.** These scripts originated in a
> particular Windows checkout, `D:\Project\Testing`, and depend on private,
> git-ignored `work/` directories and game files. Use the
> [main build and generated-game instructions](../../README.md#build-and-install)
> for a fresh checkout instead. Keep private game assets out of Git.

This directory preserves **historical game compatibility aids**, not required
source files for Zmbor's general-purpose build:

| File | Status and role |
|:--|:--|
| [`PAD_PLAYBACK.md`](PAD_PLAYBACK.md) | General scripted controller playback format; reusable runtime feature |
| [`bind_sdk_stub.py`](bind_sdk_stub.py) | Reusable SHA-1-validated ELF leaf binder; checks game/version-specific input before modifying a TOML |
| [`ppm2png.py`](ppm2png.py) | Convert captured PPM frames to PNG and report image statistics |
| [`contact_sheet.py`](contact_sheet.py) | Produce a time-series contact sheet (requires Pillow) |
| [`crash_pipeline.ps1`](crash_pipeline.ps1) | Historical single-game launcher; **hardcoded local paths**: do not run unchanged on another checkout |

Two redundant copies, `env.ps1` and `configure.ps1`, were dropped from
the versioned tree because they did not configure Zmbor generically: both
referenced a private `D:\Project\Testing\work` path. They are recoverable in
the pre-cleanup Git history. The remaining example script itself imports
an **untracked** `work/scripts/env.ps1`; use a real locally prepared
environment and adapt paths before attempting a historical reproduction.

The original [Crash engineering continuation](../../docs/engineering/CRASH_TWINSANITY_CONTINUATION.md)
documents the observed stages, regression evidence and limitations. The
[archived GPT-Sol handoff](../../docs/archive/handoffs/GPT_SOL_HANDOFF.md)
contains older workstation instructions. Neither is a public acceptance
gate for retail gameplay: introductory videos were observed, but interactive
gameplay was not verified.

**Current policy:** add reusable fixes, tests and generic instrumentation to
Zmbor's core. Keep title-specific paths, code patches, scripts and legally
supplied game data in independent compatibility projects. Preserve hashes,
compiler/build flags, timestamps and failure outcomes when publishing any
new compatibility evidence.
