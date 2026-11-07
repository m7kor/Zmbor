# Prosanity → Zmbor: engineering integration (2026-11-04)

## Provenance and scope

- Destination: [m7kor/Zmbor](https://github.com/m7kor/Zmbor), base `0ec3f4eb63df37e6b0b2bbf67420bc8e432e186a`.
- Source: [m7kor/prosanity](https://github.com/m7kor/prosanity), HEAD `798d80701a977cc0dcecef373d39d145a84f9525`.
- Original Git compare: Prosanity was 21 commits ahead of that Zmbor base, 0 behind, and changed 77 files.
- Integration method: **source-tree snapshot** of all 77 changed paths on a dedicated branch, followed by a Zmbor pull request. The original 21 commit objects remain in the Prosanity repository; this integration does **not** import their original commit ancestry. Refer to the source Git compare and SHA for commit-level provenance.
- No game files, BIOS, extracted ISO content, generated commercial-game C++, or local screenshots are imported.

## Integrated workstreams

- Stripped-ELF function/vtable discovery, recompiler and generated-code correctness, out-of-tree generated-game CMake target.
- IOP-to-EE SIF RPC server/client plumbing, CD/DVD filesystem handling, and runtime I/O safety.
- MPEG/IPU HLE improvements, FFmpeg-backed movie decode configuration, DMA, callbacks and bounded input handling.
- PAD replay support, runtime diagnostics/trace/termination/frame-series interfaces and optional profiling tools.
- Crash Twinsanity evidence documents and game pipeline tooling as a reproducible compatibility **test case**, not a game-specific fork of the core architecture.
- Regression tests, E2E updates, IOP test extensions, dashboard inventory updates and documentation.

## Validation / limits

The source repository's GitHub Actions run [41843554878](https://github.com/m7kor/prosanity/actions/runs/41843554878) passed six CI jobs on source HEAD, including Windows MSVC, Linux GCC/Clang, sanitizers, fuzz smoke and dashboard validation. This establishes source-side CI health only; verify the actual Zmbor PR checks and hardware fixtures separately before merging.

The Prosanity local engineering handoff reported 697/697 unit tests and 23/23 IOP physical-output comparisons, but those counts are historical local evidence. Crash Twinsanity reached visual startup / introductory videos; interactive gameplay and full MPEG EOF handling are **not** verified. Do not convert isolated game progress or model-level tests into PS2 hardware completeness claims.

## Forward workflow / single source of truth

**Zmbor is the canonical framework repository**. New generic EE/VU/IOP/GS/runtime fixes, acceptance gates and tools should be developed against Zmbor and merged via verified PRs. Game experiments (Crash Twinsanity, RE4 and others) remain independent compatibility clients; useful generic changes feed back into Zmbor via tests, not game-specific hardcoding. Keep the historical Prosanity repository available for provenance and reference; avoid maintaining divergent core copies.

Required gates for integration work: compile/test Windows + Linux, sanitizers where applicable, E2E and regression, hardware-reference comparisons when fixtures exist, validate the dashboard, and record performance with repeatable measurements. Never rewrite `.expected` reference files to make tests pass. Never add commercial-game assets, BIOS or locally generated game files. Never claim playability without real input-to-gameplay evidence.


## Post-merge independent review (4 November 2026)

PR #13 merged the full 77-path Prosanity snapshot into Zmbor main, including game-specific reports and
local pipeline documents. PR #14 independently staged a narrower 67-path engine-only import; it MUST NOT
be merged on top of #13 because the core code is already present. This follow-up review commits only
actual fixes against the merged main tree.

Review fix: the new IOP-to-EE RPC bridge used to validate the first and final guest addresses before
bulk `memcpy`, which did not prove a contiguous host RDRAM span. The fix applies existing
`resolveEeGuestRange` and `getEeGuestStruct` validators, and prevents returned reply ranges from
wrapping to unrelated guest RAM aliases. A guard-padded near-RAM-end/KSEG1/normal-path regression is
added to `ps2_sif_rpc_tests.cpp`. The general PAD/profiling documentation is also made independent
of local Crash Twinsanity work scripts, and the trace header's dead document reference is corrected.

Validation gates: run the new follow-up branch CI; rerun complete EE 55 and IOP 23 hardware-reference
suites, gameplay/transition tests for RE4 and Crash (with user-owned files), and matched-workload
performance before claiming universal compatibility or a performance gain. Do not edit .expected,
force-push/rewrite public history, or publish ISO/BIOS/generated commercial game code.
