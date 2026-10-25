# Sanitizers, fuzzing and integer proofs

## Builds

| build | configure | purpose |
|---|---|---|
| ASan (MSVC) | `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS2X_SANITIZE=address` | unit suite and runners under AddressSanitizer |
| ASan + UBSan (Clang/GCC) | `-DPS2X_SANITIZE=address,undefined` | the same plus undefined behaviour (CI `linux-clang-asan-ubsan`), e2e generated code included |
| fuzz | `-DPS2X_SANITIZE=address -DPS2X_BUILD_FUZZERS=ON` | libFuzzer harnesses in `tools/fuzz` |

Instrumentation is global (fetched dependencies too). Use RelWithDebInfo: `EnableFastReleaseMode` adds `/GL` and
`/fp:fast` for Release only. On MSVC the coverage symbols live in the libFuzzer runtime, so a fuzz build turns off
`ps2x_tests` and the analyzer; run the unit suite from an ASan-only build. MSVC ASan needs
`clang_rt.asan_dynamic-x86_64.dll` on `PATH` (it is in `VC\Tools\MSVC\<ver>\bin\Hostx64\x64`). Some unit tests read
repository files relative to the working directory: run `ps2x_tests` from the repository root.

## Harnesses (`tools/fuzz`)

| target | entry | input |
|---|---|---|
| `ps2x_fuzz_gif` | `GS::processGIFPacket` | arbitrary GIF packet (tags, PACKED/REGLIST/IMAGE, A+D register writes) |
| `ps2x_fuzz_vif0` | `PS2Memory::processVIF0Data` | VIF0 code stream (UNPACK, MPG into VU0 micro memory, MSCAL) |
| `ps2x_fuzz_vif1` | `PS2Memory::processVIF1Data` + pending transfers | VIF1 stream, DIRECT/DIRECTHL to PATH2, MSCAL |
| `ps2x_fuzz_dma` | DMAC registers (`D2_TADR/QWC/CHCR`) | 4-byte tag address + 1 MB RDRAM window; chain DMA on the GIF channel |
| `ps2x_fuzz_elf` | `ElfParser::parse` | arbitrary file |
| `ps2x_fuzz_decoder` | `R5900Decoder` + `CodeGenerator::generateFunction` | instruction words, decoded, disassembled and emitted as one function |

The runtime harnesses keep one `PS2Memory`+`GS` across inputs (as frames would) and rebuild them every 4096 inputs.
Seeds are generated, never committed game data: `python -I tools/fuzz/make_seeds.py <dir>`. A crash found by a
fuzzer is fixed at the root cause; its minimized input goes to `tools/fuzz/regressions/<target>/` (replayed by CI)
and, where the defect is reachable from the API, a unit test.

## Integer proofs (`tools/semantics/z3_int_semantics.py`)

Z3 bit-vector proofs that the overflow predicates of `ADD32_OV/SUB32_OV/ADD64_OV/SUB64_OV` (ADD, ADDI, SUB, DADD,
DADDI, DSUB), DADDIU's immediate extension, `ps2_plzcw32`, the RDRAM fast-path bounds and the DIV guards equal an
independent specification for every input; every proof has a negative control that must be refuted. ctest
`z3_int_semantics` (skipped, not passed, when z3 is missing; CI installs it). Integer only: nothing here says
anything about floating-point semantics.

## Findings

### F1 — signed-overflow UB in overflow-trapping adds/subtracts (fixed)

`ADD32_OV`/`SUB32_OV` and the emitted DADD/DSUB/DADDI computed `int32_t`/`int64_t` sums, and DADDIU an `int64_t`
sum. A signed overflow is undefined behaviour in C++, and overflow is the very case these instructions test. Evidence:
`ADD32_OV(0x7FFFFFFF, 1)` compiled with clang 23.1.1 `-O2 -fsanitize=signed-integer-overflow
-fsanitize-trap=signed-integer-overflow` traps (exit 132); uninstrumented it happened to produce the right result
(`r=80000000 ov=1`), i.e. the generated code was correct only by the compiler's choice. Fix: unsigned arithmetic and
a sign-bit mask; the predicates are proved (above); the instrumented program no longer traps and prints the same
result. Regression: `code_generator_tests` (emitted DADD/DSUB/DADDI/DADDIU use the unsigned forms),
`ps2_runtime_expansion_tests` (boundary pairs for all four macros), CI UBSan job.
