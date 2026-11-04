# Prosanity → Zmbor: post-merge engineering review (2026-11-04)

## Context
- Canonical target: `m7kor/Zmbor`, merged PR [#13](https://github.com/m7kor/Zmbor/pull/13), main SHA `6c6249519badcb16d1da2e46cfcba0a790736fcd`.
- Source Prosanity snapshot: `798d80701a977cc0dcecef373d39d145a84f9525`.
- Second overlapping PR [#14](https://github.com/m7kor/Zmbor/pull/14) must not merge the already imported core a second time.
- Baseline import CI: 6/6 jobs passed on PR #13's imported code. Tests do NOT prove games playable.

## Findings and corrective action
1. **High — IOP→EE RPC invalid host bulk copy:** The initial code checked that both the first and last guest
   pointers mapped, then called `memcpy` on the full `send.size()` span. For a buffer near the end of EE RDRAM,
   the pointer at the far end could resolve through an alias, while `memcpy` would write past the physical host RAM.
   Use the existing whole-span guest range validator and reject the request before copying. The server descriptor
   itself now requires a complete valid EE guest struct. Test checks rejection at end of RAM and through KSEG1,
   unchanged host guard bytes, and successful copying to a valid buffer.
2. **Medium — RPC reply guest-address wrap:** An unsigned 32-bit reply address plus index could wrap or cross
   an EE memory alias while constructing reply bytes. Limit source reply bytes to the contiguous mapped RAM or
   scratchpad region. Preserve bounded partial replies without crossing the guest region.
3. **Low — imported tooling docs depended on excluded/local scripts:** PAD playback and Windows profiler
   examples used a specific Crash checkout and handoff scripts. Replace these with standalone, general examples.
4. **Process — duplicate import PRs:** PR #13 already merged; #14 must be retired or repurposed. Only
   independent regression fixes should be proposed against the current `main`.

## What has been inspected
- All 77 changed paths in the original import compared against the source; 67 common code/tooling paths
  also assessed against the narrower engine import.
- Focused static review of SIF/IOP/EE RPC, ELF discovery, CD/VFS, MPEG/IPU HLE, PAD playback, runtime
  tracing, profiling and FFmpeg CMake configuration, plus tests/documentation.
- New issue-driven regression added to `ps2xTest/src/ps2_sif_rpc_tests.cpp`.

## Gates still open
- Zmbor CI on this follow-up commit (Windows MSVC, GCC, Clang, sanitizers and fuzz smoke).
- Hardware corpus EE 55/55 checked against original references; known original baseline had 53/55 exact.
- IOP hardware corpus 23/23 rerun with original reference outputs.
- RE4 compatibility smoke/room transitions and Crash Twinsanity intro-to-interactive input/scene with real assets.
- Performance measurements with disabled verbose logging and identical workloads.
- Review personal workstation traces and handoff documents before treating this as a generally distributable SDK.

## Scope boundary
No BIOS, ISO, game assets or local diagnostics are introduced by this review. No game playability claim
is made; static correctness review plus hosted CI is not a real-PS2 or retail-game compatibility proof.
