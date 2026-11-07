# Open-source technology evaluation

This pass triages candidates by intended use. The [existing landscape](../docs/research/OPEN_SOURCE_TECHNOLOGY_LANDSCAPE.md), [feasibility matrix](../docs/research/INTEGRATION_FEASIBILITY_MATRIX.md) and [benchmark evidence](../docs/research/TECHNOLOGY_BENCHMARKS.md) contain earlier experiments. Recheck each candidate's revision, SPDX and transitive requirements before adoption.

| Use | Candidates | Decision and evidence needed |
| --- | --- | --- |
| Binary analysis | Ghidra/EE extension, ELFIO, LLVM/Clang, Capstone, angr, Z3 | Keep ELFIO in process and Ghidra as an external optional oracle. Check decoder coverage for R5900 MMI and VU before considering substitution. Use Z3 and angr offline and measure precision, runtime and provisioning costs. |
| Hardware | PCSX2, Play!, ps2sdk, ps2toolchain, ps2autotests | Compare explicit observations and use pinned homebrew tooling to settle disagreements. Keep external fixtures and unchanged expectations. Investigate exact licensing of each imported fixture or table. |
| GS and profiling | Vulkan, Mesa, SDL3, RenderDoc, Tracy, shader validation tools | Use existing software GS and optional Vulkan adapter for comparison. A backend choice requires frame and VRAM parity plus timing; measure profiling overhead separately. SDL3 and Mesa integration remain deferred. |
| Audio | miniaudio, SDL audio, OpenAL Soft, SPU2 references | Measure deterministic guest mixing separately from host buffering. Compare latency, drift, underruns, device recovery and platform coverage before backend selection. |
| Validation | CMake, Ninja, CTest, MiniTest, GoogleTest, Catch2, libFuzzer, ASan, UBSan, TSan, clang-tidy, clang-format, CodeQL, GitHub Actions | Keep the existing test runner and CI. Pin dependency inputs and extend the most relevant gates. TSan and static tools need platform-specific smoke runs; a new C++ test framework needs a concrete benefit. |

No new library was integrated in this cycle. Maintenance, cross-platform costs and licenses outside the pinned first-six comparison remain to be verified.

## Interface checks

- Ghidra's headless API supports analysis followed by post-scripts and reports analysis timeouts. Calls within one process require serialized access. The existing separate-process exporter can preserve the timeout and revision in its output provenance before Zmbor validates candidate addresses. [Ghidra API](https://ghidra.re/ghidra_docs/api/ghidra/app/util/headless/HeadlessAnalyzer.html)
- LLVM permits reordering volatile operations relative to ordinary operations, and its arithmetic flags can introduce poison. An EE lowering experiment needs explicit device effects and guest-width arithmetic rules before enabling optimizations across memory and exception boundaries. [LLVM IR reference](https://llvm.org/docs/LangRef.html)
- SDL3 audio streams support queued and callback-driven PCM with explicit device lifetime. A host adapter can be evaluated with an identical deterministic SPU2 signal and measured queue depth. Guest 48 kHz scheduling stays under the emulated clock. [SDL3 audio API](https://wiki.libsdl.org/SDL3/SDL_OpenAudioDeviceStream)
