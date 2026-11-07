# PS2 recompilation approaches

The [pinned source matrix](COMPETITOR_TECHNICAL_MATRIX.md) records the first six deep-dive repositories.

## Discovery and dispatch

Zmbor and PS2Recomp use an ELFIO-backed parser and function-oriented C++ emission. Zmbor also has Ghidra-map ingestion, evidence from control-flow analysis and address-taken entries. The web recompiler begins from loadable segments and promotes tentative traversal candidates iteratively. Its compact entry table supports relocatable module instances. Native recomp adds a separate compiler-backed miss path for dynamic images.

The proposed comparison uses identical synthetic stripped ELFs and scores true starts, false starts and missing reachable indirect edges. For overlays, include two images at the same address and preserve both identities in the expected trace. Record translation time and emitted bytes independently of semantic parity.

## Runtime and automation

The native fork separates disc parsing, boot executable resolution and a native build. Its [disc test fixtures](https://github.com/exoteric33/ps2-native-recomp/tree/fe6505dee0001e896b6f65e8b1316722dcd9ac9b/tests) include BOOT2, dual-layer and BIN/CUE cases. These provide fixture categories for Zmbor's title-independent pipeline. Zmbor has a [title pipeline](../tools/game_pipeline/README.md) and generated ELF-to-native e2e tests that can anchor the work.

PCSX2 is a behavioral reference for devices and scheduling. It and the PS2 static projects use different execution models, so a speed comparison needs equal guest workloads, equivalent accuracy settings and matched output. Current game compatibility and performance rankings remain unknown.
