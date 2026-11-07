# Prioritized technical debt and roadmap

Priority reflects observed impact and dependencies. “Gap” includes missing evidence where the repository already implements a feature.

| Rank | Gap and type | Next independently testable gate |
| --- | --- | --- |
| 1 | ELF metadata can parse successfully while instruction payload byte order is outside the EE contract (incorrect admission). | Fail both shared-parser consumers before discovery on a synthetic ELF32 MSB input. Exercise an equivalent LSB input. |
| 2 | Malformed load segments and agreement across the shared parser and runtime loader (partially addressed). | Initial coverage warnings report unverified section bytes. Correlate those ranges with observed runtime loads before attributing them to overlays or unsupported mappings. |
| 3 | Stripped ELF target recovery (architectural limitation). | Score existing symbol, metadata, Ghidra and inferred candidates separately against known starts and reachable edges. Report false positives. |
| 4 | Code overlays and late indirect targets (missing general functionality). | Record unresolved target, guest PC and image identity, then replay a synthetic overlay with two versions at the same address. |
| 5 | EE exceptions, COP1 and VU EFU agreement (incomplete hardware verification). | Rerun the exact-output corpus, followed by discriminating PS2 probes for unresolved discrepancies. |
| 6 | SIF and device ordering (incomplete hardware verification and integration). | Compare DMA/IRQ traces on a deterministic EE/IOP fixture before changing scheduler policy. Check the current component matrix for implemented portions. |
| 7 | CPU/GPU GS parity (incomplete testing and unmeasured performance). | Replay identical GIF inputs through software and the optional Vulkan adapter, with frame, VRAM and timings captured together. |
| 8 | SPU2 audio fidelity and host scheduling (incomplete testing). | Compare deterministic voice/loop/IRQ output before measuring host underruns and drift. |
| 9 | Reusable disc-to-host conversion (architectural limitation). | Automate a synthetic ISO boot ELF through compile and headless run, with per-stage outcomes and reproducible inputs. |
| 10 | Compatibility and release evidence (incomplete testing and documentation). | Record per-fixture milestone, host, input hashes, exit or interaction evidence and matching build ID. Repeat clean builds with pinned dependencies. |

| Phase | Dependencies | Implementation and exit evidence |
| --- | --- | --- |
| 0 Baseline | Clean pinned source | Build Windows and Linux configurations, record test counts and measured workloads. Store logs and dependency identities. |
| 1 Research | Source access | Review pinned implementations and licenses. Compare candidates with the same fixture or label source-only conclusions. |
| 2 Correctness | 0–1 | Regress ELF admission, payload bounds, generated control flow and CPU instructions. Require before/after failure evidence and sanitizer coverage where supported. |
| 3 Compiler | 2 | Add target provenance and trace/replay. Measure candidate precision, generated size, compile/link time and execution parity before introducing intermediate operations. |
| 4 Devices | 2 | Model EE/VU/DMA/IOP event ordering. Gate changes with repeatable trace comparisons and the hardware corpus. |
| 5 Host | 4 | Compare GS frames and SPU2 signals, then validate host input, disc and save behavior. Track frame variance, underruns and drift under the same inputs. |
| 6 Conversion | 3–5 | Run a synthetic ISO through extraction, analysis, translation, build and execution. Exit on structured success and injected failure cases for every stage. |
| 7 Compatibility | 6 | Apply the existing [compatibility matrix](../architecture/compatibility-matrix.md) to reproducible homebrew and authorized title workloads. Separate bugs from reached milestones. |
| 8 Optimization | Measured correctness gates | Use repeated same-host workloads to profile dispatch, memory, GS, audio and build time. Require identical correctness results and a speed change above observed variance. |
| 9 Developer experience | Stable CLI contracts | Surface traces and evidence in Studio through shared analysis/runtime APIs. Verify that CLI and UI report the same fixture outcomes. |
| 10 Release | 0–9 | Pin dependencies, build supported configurations in CI, verify package contents and publish scoped evidence. |

Sources and design constraints live in the [reference comparison](../../research/COMPETITOR_TECHNICAL_MATRIX.md), [hardware dependency map](PS2_HARDWARE_DEPENDENCIES.md) and [incremental recompiler design](../architecture/NEXT_GENERATION_RECOMPILER_DESIGN.md).

The Windows Release CLIs previously returned exit 0 for the eight-byte `PT_LOAD` in `elf_byte_order.py` when `p_memsz` was four. The runtime regression uses a valid segment followed by an invalid one. Before the fix, `loadELF` copied the first segment before rejecting the second when `p_memsz` was four, and returned success when it was zero. The shared parser now rejects both cases before discovery. The runtime checks and retains the program headers before setting the guest PC or copying segments. A positive CLI fixture exercises an initialized segment followed by its zero-filled tail. [ELF program loading specification](https://gabi.xinuos.com/elf/07-pheader.html#segment-types)

The overlap probe used a sectionless ELF with an eight-byte executable segment at `0x00100000` and a four-byte executable segment at `0x00100004`. Replacing the second segment's NOP with `addiu $v0, $zero, 7` still produced byte-identical translated C++. The runtime regression confirmed the parser read `0x00000000` at the overlap while the loaded guest image held `0x24020007`. The shared parser checks the overlapping file bytes and zero-filled tails when program headers supply its bytes. Direct-mapped KSEG0/KSEG1 and uncached mirrors use a common comparison address for spans that fit in RDRAM. Matching contents are accepted. Disagreements are refused before discovery regardless of header order. The desktop loader retains its program-header-order mapping. Section-backed sources, permissions, scratchpad aliases and TLB mappings require separate validation. [ELF program loading specification](https://gabi.xinuos.com/elf/07-pheader.html#segment-types)

The scratchpad regression found that both runtime loaders wrote aliased scratchpad segments into low RDRAM. Both now select scratchpad through the same address predicate as ordinary memory accesses and validate its extent in their header pass. The sectionless comparison also normalizes the alias for ranges that fit in scratchpad.

Both runtime loaders validate the full list of destinations before copying segment payloads. The primary loader preserves the prior entry PC and I/O paths when a destination fails validation. The SIF loader rejects inconsistent ELF class or byte-order markers and reports an unmapped TLB destination through the syscall result. A read failure after successful preflight, such as a file changing during the copy, can still leave partial writes. The primary loader also publishes the image before ROM profile selection, so an ambiguous profile can fail after publication. Section-backed data sources, permissions and segments that span TLB mappings remain separate cases.

The section-backed EE code check uses the last program-header write at each mapped address, including zero-fill. The regression found that a stale section was accepted despite a different instruction in guest RAM. Matching the final overlay is sufficient even if an earlier segment supplied different bytes. An executable code section claiming a file payload also needs readable bytes. Section-backed data and executable bytes outside load extents still need separate coverage.

Allocated file-backed data now uses the same image check before static discovery. The regression observed a false candidate from a singleton pointer at `0x00100200` that the final segment zeroed in RDRAM. The agreeing overlay still discovers the target at `0x00100040`. Sections without comparable load coverage and changes made to data after the initial load remain separate cases.

The section policy reports partial and absent initial coverage as warnings with the virtual address and verified byte count. Analysis continues because an uncovered range alone cannot establish how a title populates it. A disagreement in covered bytes remains fatal. Future runtime load traces can distinguish later overlays from ranges with no observed backing.

`ZMBOR_TRACE_LOAD=1` records copied SIF ELF segment ranges as `elf:sif-segment` events. Each records the caller PC, virtual base, destination region and offset, payload size and total memory size. The event describes the completed write, even if a later segment fails. The existing per-key event limit applies to the shared segment key. Raise `ZMBOR_TRACE_EVENTS` when collecting multiple segments and treat a capped trace as incomplete.

`tools/compat/correlate_loads.py` compares exact initial gaps with the union of recorded writes in each memory region. With complete SIF event details, it can compare fingerprints for an exact final write or, with the original ELF, for several intact writes that together cover the gap. Reconstructing bytes from partially overwritten writes, resolving TLB-backed ranges and observing DMA or custom overlay copies remain separate tasks.
