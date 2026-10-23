# Zmbor

**The Next Generation PlayStation 2 Recompilation Framework**

Zmbor is a framework for PlayStation 2 static recompilation, hardware runtime and native game reconstruction. It
turns PS2 ELF binaries into C++ and runs them natively against a runtime that models the Emotion Engine, VU0/VU1,
VIF, GIF/GS, DMAC and IOP.

> [!IMPORTANT]
> Compatibility with commercial PS2 games has **not** been verified. The accuracy figures below come from hardware
> test programs, unit tests and synthetic end-to-end programs, not from retail games. Zmbor ships no game data or
> BIOS, and you must supply your own legally obtained software.

### Relationship to PS2Recomp

Zmbor is built on [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp), forked at `eb8688b`, with upstream #384
merged, and keeps its full Git history, module layout, target names and GPL-3.0 license. It adds a long engineering
effort on accuracy, testing and integration: the "PS2Recomp Ultimate" work, described in `CHANGELOG.md` and
`HANDOFF.md`. The upstream PS2Recomp documentation follows below and still applies.

### Verified hardware accuracy

The [ps2autotests](https://github.com/unknownbrackets/ps2autotests) suite (ISC) provides 55 ELFs whose expected
output was printed by a real PS2. Zmbor recompiles each one to native code and compares its output line by line
(`tools/hwtest/hwtest.py`, dashboard in `docs/hwtest/`):

| | exact tests | matching lines |
|---|---|---|
| upstream base | 6 / 54 | 5346 / 8240 (65%) |
| Zmbor | **47 / 55** | **8172 / 8250 (99.1%)** |

Fully matching areas are `kernel/ee` 10/10, `dma` (VIF, SPR, DMAC) 11/11, `cpu/ee_simd` 6/6, `gs` 2/2 and `vu` 6/7.
There are no crashes or timeouts. These numbers were measured just before upstream #384 was merged; after the merge,
the unit, ctest and IOP suites were rerun, but the hardware suite was not. The 8 remaining failures, with their causes,
are listed in `HANDOFF.md`.

### What Zmbor adds

* **Recompiler:** exact 64-bit branch conditions, branch-likely links, the COP0 DMAC condition (BC0), delay-slot-safe
  function discovery, MMI multiplies and 128-bit HI/LO, SA semantics.
* **EE runtime:** PS2 float semantics, rounding and flags; kernel thread and interrupt fixes.
* **VU0/VU1:** microprogram start from VIF and COP2; correct EE-visible VU memory; EFU and FMAC arithmetic fitted to
  real-PS2 vectors.
* **VIF / GIF / DMA:** full UNPACK (all formats, CL/WL, STMOD, masks, split transfers), I-bit stalls, scratchpad
  chain and interleave DMA, tag interrupts.
* **Testing:** automated hardware result pipeline, end-to-end ELF → C++ → native tests with GS frame checks, a GS
  cache regression suite and IOP tests.
* **Community research:** findings from 15 PS2Recomp game projects and 205 forks, collected in a validated database
  (`research/`, `docs/community-research/`); 12 fixes integrated.

### Build and test

```bash
git clone https://github.com/m7kor/Zmbor.git
cd Zmbor
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_IOP_BUILD_TESTS=ON
cmake --build build
build/ps2xTest/ps2x_tests                      # unit tests (run from the repository root)
ctest --test-dir build/ps2xTest -LE hwtest     # research DB, GS cache, end-to-end
ctest --test-dir build/ps2xIOP
```

See `TESTING.md` for every test level, including the hardware result tests. The build has been tested on Windows 11
with MSVC 14.44 and Ninja; other platforms are untested.

### Limitations

* No commercial game has been run end to end or verified.
* The EFU series (EATAN/ESIN/EEXP) and the FPU MUL.S/RSQRT.S are not bit-exact. Branches inside delay slots, COP0
  performance counters and the guest heap limit are not modelled correctly yet.
* VU and GS performance is still low.

The roadmap is in [ROADMAP.md](ROADMAP.md).

### Credits

Zmbor builds on PS2Recomp by ran-j and its contributors (see the upstream acknowledgments below). Hardware reference
results come from ps2autotests by unknownbrackets and contributors (ISC). PCSX2 was consulted as a reference only, and
no PCSX2 code was copied.

---

## PS2Recomp: PlayStation 2 Static Recompiler (Experimental) — upstream documentation

[![Discord](https://img.shields.io/badge/Discord-Join%20Server-5865F2?logo=discord&logoColor=white)](https://discord.gg/JQ8mawxUEf)

Also check the upstream [WIKI](https://github.com/ran-j/PS2Recomp/wiki)


This project statically recompiles PS2 ELF binaries into C++ and provides a runtime to execute the generated code.

### Modules

* `ps2xAnalyzer`: scans ELF/functions and writes TOML config (`stubs`, `skip`, instruction patches).
* `ps2xRecomp`: reads TOML + ELF, decodes R5900 instructions, and generates C++ output.
* `ps2xRuntime`: hosts memory, function registration, syscall dispatch, and hardware stubs.
* `ps2xIOP`: R3000A IRX execution, a virtual IOP kernel, and generic HLE fallbacks.

### Features

* Translates MIPS R5900 instructions to C++ code
* PS2-specific MMI and VU0 macro support.
* Single-file or multi-file output.
* Configurable stubs, skips, and instruction patches.
* Instruction-driven syscall handling.

### How It Works
PS2Recomp works by:

* Parsing a PS2 ELF file to extract functions, symbols, and relocations
* Decoding the MIPS R5900 instructions in each function
* Translating those instructions to equivalent C++ code
* Generating a runtime that can execute the recompiled code

The translated code is very literal, with each MIPS instruction mapping to a C++ operation. For example, `addiu $r4, $r4, 0x20` becomes `ctx->r4 = ADD32(ctx->r4, 0X20);`.

### Current Behavior

* `stubs` entries generate wrappers that call known runtime syscall/stub handlers by name.
* `stubs` also supports address bindings with `handler@0xADDRESS` for stripped games (for example `sceCdRead@0x00123456`).
* Address bindings also support generic return handlers for triage: `ret0`, `ret1`, `reta0`.
* Recompiler now tries relocation-symbol auto-binding at callsites (`J/JAL`) before raw address dispatch; when relocation symbol is known (for example `sceCdRead`), it can call runtime handlers without manual address mapping.
* Recompiler discovers additional internal static entry targets and emits `entry_...` wrappers for those addresses.
* For unresolved static `J/JAL` sites, generated code falls back to `runtime->lookupFunction(0x...)`.
* `skip` entries are not recompiled and generate explicit `ps2_stubs::TODO_NAMED(...)` wrappers.
* Recompiled `SYSCALL` now calls `runtime->handleSyscall(...)` with the encoded syscall immediate.
* Runtime syscall dispatch tries encoded syscall ID first, then falls back to `$v1`.

### Requirements

* CMake 3.20+
* C++20 compiler (currently tested mainly with MSVC)
* SSE4/AVX host support for some vector paths

### Build

```bash
git clone --recurse-submodules https://github.com/ran-j/PS2Recomp.git
cd PS2Recomp

cmake -S . -B out/build
cmake --build out/build --config Debug
```

### Usage

Preferred workflow for retail or stripped games:

1. Open the ELF in Ghidra.
2. Run `ps2xRecomp/tools/ghidra/ExportPS2Functions.java`.
3. Use the exported TOML and CSV map.
4. Recompile with the exported TOML:

```bash
./ps2_recomp config.toml
```

Fallback workflow for quick local experiments or ELFs with debug symbol :

```bash
./ps2_analyzer your_game.elf config.toml
```

See the [Ghidra Workflow](ps2xAnalyzer/Readme.md#3-ghidra-integration-for-retail-and-stripped-games-preferred) for ghdira instructions.

Then build generated output and link with `ps2xRuntime`.

### Configuration

Main fields in `config.toml`:

* `general.input`: source ELF path.
* `general.ghidra_output`: recommended function map CSV exported from Ghidra.
* `general.output`: generated C++ output folder.
* `general.single_file_output`: one combined cpp or one file per function.
* `general.low_memory_mode`: reduce peak output-generation memory by avoiding retained disassembly strings and forcing serial output generation. Generated instruction comments are still emitted; disassembly text is produced while writing each output file instead of being kept in memory.
* `general.output_worker_threads`: number of output-generation workers (clamped to nproc * 2). A positive value uses exactly that many workers. `0` uses `nproc - 1` when at least two hardware threads are available, otherwise serial output generation. `1` forces serial output generation.
* `general.patch_syscalls`: apply configured patches to `SYSCALL` instructions (`false` recommended).
* `general.patch_cop0`: apply configured patches to COP0 instructions.
* `general.patch_cache`: apply configured patches to CACHE instructions.
* `general.stubs`: names to force as stubs. Also accepts `handler@0xADDRESS` to bind a stripped function address directly to a runtime syscall/stub handler. Includes generic handlers `ret0`, `ret1`, `reta0`.
* `general.skip`: names to force as skipped wrappers.
* `patches.instructions`: raw instruction replacements by address.

Address binding for stripped ELFs:

* Use `handler@0xADDRESS` inside `general.stubs` to map a stripped function start directly to a runtime handler.
* Example: `sceCdRead@0x00123456` binds function start `0x00123456` to `ps2_stubs::sceCdRead(...)`.
* Generic temporary handlers are available: `ret0@0xADDR`, `ret1@0xADDR`, `reta0@0xADDR`.
* Before manual binding, prefer recompilation from a Ghidra-exported TOML/CSV first. The extra boundaries and synthetic entry points are usually more important than manual early triage.
* The address must be the function start in that exact ELF build.
* Addresses are not portable across different games/regions/builds.
* The handler name must exist in runtime call lists (`PS2_SYSCALL_LIST` or `PS2_STUB_LIST`).

Example:

```toml
# stripped function binding by address:
stubs = ["sceCdRead@0x00123456", "SifLoadModule@0x00127890"]
# temporary return handlers:
stubs = ["ret0@0x001D9410", "ret1@0x001D5BC8", "reta0@0x0024B7C0"]
# mixed example:
stubs = ["printf", "sceCdRead@0x00123456", "SifLoadModule@0x00127890"] 
```

### Runtime

To execute the recompiled code.

`ps2xRuntime` currently provides:

* Guest memory model and function dispatch table.
* Some syscall dispatcher with common kernel IDs.
* Basic GS/VU/file/system stubs.
* Foundation to expand and port your game.
* `ps2xIOP` execution of original IRX modules with generic HLE fallbacks.

See [IOP emulation](ps2xIOP/README.md) for module execution and the service boundary.

### Game Override Hooks

Game overrides are runtime-side, build-scoped patch modules.

A game override is C++ code that runs during `loadELF` and can replace EE function bindings by address for one specific game build. IOP RPC/DMA behavior is handled by the `ps2xIOP` emulator and its runtime transport. This is separate from recompilation output and separate from global runtime stubs/syscalls.

API:

* Header: `ps2xRuntime/include/game_overrides.h`
* Register macro: `PS2_REGISTER_GAME_OVERRIDE(name, elfName, entry, crc32, applyFn)`
* Direct bind helper: `ps2_game_overrides::bindAddressHandler(runtime, addr, "handler")`

Use Game Override modules when:

* You need per-game/per-build routing or patches without polluting global behavior.
* You need to bind many addresses, or install custom replacement logic for a specific title.

#### Recommended Iteration Loop

1. Run with minimal config and no aggressive skipping.
2. Fix hard blockers first (`function not found`, syscall TODO, critical IO stubs).
3. Use temporary return stubs only to classify call importance.
4. Promote temporary fixes to real implementations.
5. Move per-game hacks into game overrides keyed by ELF metadata.
6. Re-test from cold boot after each batch.

### Describing Your Game Project

A game project built with PS2Recomp can describe itself in a `.recomp.json` file at the root of its repository. Lists of recomp and decomp projects, such as [recomp.board](https://recomp.fyi), read that file instead of guessing the game, system and status from the README.

> [!NOTE]
> This file is optional: PS2Recomp does not read it and works the same without it. `.recomp.json` and recomp.board are third-party projects; the PS2Recomp developers have no ties to them.

Starter file:

```json
{
  "$schema": "https://recomp.fyi/schema/v1.json",
  "game": "<title as it shipped>",
  "system": "PS2",
  "type": "recomp",
  "toolchain": "PS2Recomp",
  "status": "in-progress",
  "original": { "region": "USA", "serial": "SLUS-20312" }
}
```

* `original` is the release a user must own. On retail discs the ELF is named after the serial (`SLUS_203.12` is `SLUS-20312`), and the prefix gives the region: `SLUS`/`SCUS` USA, `SLES`/`SCES` Europe, `SLPS`/`SLPM`/`SCPS` Japan.
* `status` is one of `exploring`, `in-progress`, `playable`, `released`, `complete`, `paused`. Edit it when the project moves on: a stale status is worse than none.
* Never put a game file, or a link to one, in the file.

Other fields (Wikidata item, target platforms, maintainers, links, what help is wanted) and the JSON Schema are in the [specification](https://recomp.fyi/spec).

### Limitations
 
* Performance is very bad for VU and GS
* Hardware emulation is partial and many paths are stubbed.

###  Acknowledgments

* Inspired by N64Recomp
* Uses ELFIO for ELF parsing
* Uses toml11 for TOML parsing
* Uses fmt for string formatting
* Reference for runtime PCSX2