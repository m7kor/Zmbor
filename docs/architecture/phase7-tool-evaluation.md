# Phase 7 — open-source tool evaluation

Revisions and licenses were read on 2026-10-25 from the GitHub API (`repos/<r>`, `repos/<r>/license`) and
`git ls-remote <r> HEAD`. Where the API reports `NOASSERTION`, the license named below is the one stated in the
repository's own LICENSE file. "Used" means the tool ran in this phase; "reference" means its documentation or
public behaviour was consulted; nothing was copied from any project.

Zmbor itself is derived from PS2Recomp (GPL-3.0), so GPL-3.0 code would be license-compatible. It is still not
copied: an emulator's code mirrors that emulator's architecture and its own errors, and Zmbor's rule is that
semantics come from hardware evidence (ps2autotests `.expected`), not from another implementation.

| project | license | HEAD / latest release | decision | reason |
|---|---|---|---|---|
| PCSX2 | GPL-3.0 | `c6cc5160f914` / v2.8.2 | reference only | Behaviour reference when triaging; never evidence, never copied (standing rule). |
| PS2Recomp (upstream) | GPL-3.0 | `e5636981a1c9` | reference | Upstream of Zmbor; upstream fixes may be compared, not merged blindly. |
| ps2autotests | ISC | `97469ffbed86` (last push 2019) | **used** | The 55 hardware programs with real-PS2 `.expected` files are the hardware gate. Unchanged. |
| ps2sdk | AFL-2.0 | `329e2bb43287` | not adopted (blocked) | Needed to build new hardware probes; no toolchain installed and no PS2 to run them on. |
| parallel-gs | LGPL-3.0 | `cc6184af7e0c` | spike deferred | Vulkan GS. A spike needs a Vulkan device and GS traces from real programs to compare against; neither a Vulkan test harness nor reference GS dumps exist here. The CPU GS backend stays the reference path. LGPL-3.0 would require dynamic linking or source availability of the library. |
| ghidra-emotionengine-reloaded | Apache-2.0 | `fcc5deac45d8` / v2.1.38 | not adopted | Interactive disassembly aid; no automated benefit for the gate. |
| spimdisasm | MIT | `4b5d4c68eeb9` / 1.42.4 | candidate (not adopted) | Could serve as an independent differential decoder for the R5900 decoder fuzzer (both disassemble the same word). Requires Python + rabbitizer; Zmbor already uses rabbitizer, so the independence would be partial. |
| FuzzTest | Apache-2.0 | `d14b2168d997` / 2026-06-29 | not adopted | Requires Bazel or a CMake build with Abseil/RE2 and works best with Clang on Linux; libFuzzer harnesses give the same coverage-guided search without new dependencies. |
| AFL++ | AGPL-3.0 (repo license field) | `d0070729206a` / v5.03c | not adopted locally | Linux-only in practice. The libFuzzer harnesses can be run under AFL++'s libFuzzer driver later without changes. Used only as an external tool, never linked. |
| libFuzzer (LLVM / MSVC) | Apache-2.0 with LLVM exception | MSVC 14.44, clang 23.1.1 | **used** | Ships with MSVC (`/fsanitize=fuzzer`) and clang. Six harnesses in `tools/fuzz`. |
| AddressSanitizer / UBSan | Apache-2.0 with LLVM exception | MSVC 14.44, clang 23.1.1 | **used** | `PS2X_SANITIZE=address` (MSVC), `address,undefined` (GCC/Clang). |
| Z3 | MIT | `a2fae73358e4` / z3-5.3.1 (local Python binding 4.13.0) | **used** | Bit-vector proofs of integer instruction semantics (Step 3). Integer proofs only: no floating-point equivalence is claimed from them. |
| cvc5 | BSD-3-Clause (optional GPL components) | `c2887c45685d` / cvc5-1.4.2 | not adopted | Second solver only adds value for theories Z3 handles poorly (e.g. FP with non-IEEE semantics); the PS2 FPU is not IEEE, so neither solver's FP theory models it without a custom encoding. |
| Tracy | BSD-3-Clause | `78f0da621e0e` / v0.14.1 | not adopted | Instrumentation profiler; the existing `tools/bench` sampling profiler answered the Phase 6 questions without source instrumentation. Candidate if frame-level profiling of real programs becomes possible. |
| RenderDoc | MIT | `e99d699ccd8d` / v1.46 | not adopted | Graphics API debugger; the GS reference path is CPU rasterisation, which RenderDoc does not see. Useful only after a GPU GS backend exists. |
