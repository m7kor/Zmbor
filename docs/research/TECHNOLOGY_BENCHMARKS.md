# Zmbor Technology Benchmarks

Research program deliverable 3 of 4. Companion documents: `OPEN_SOURCE_TECHNOLOGY_LANDSCAPE.md`
(landscape), `INTEGRATION_FEASIBILITY_MATRIX.md` (decisions), `../architecture/NEXT_GENERATION_RECOMPILER_DESIGN.md`
(design). This document contains every measurement made in the program, with commands,
environment, and raw outputs or pointers to them. **Claims that were not measured are labelled
as such — they are not benchmarks.**

## 0. Environments (read before comparing any two numbers)

| Measurement | Environment | Sharing / caveat |
|---|---|---|
| POC 1 (Ghidra headless) | OpenJDK 21.0.12.1, Ghidra 12.1.4, EE extension manually built from source HEAD `4c787f8` (v2.1.39+head); constrained sandbox VM (see POC 1 README for the Gradle failure and manual-build procedure) | Whole headless runs took minutes wall-clock, dominated by JVM startup; **no precise per-fixture timing was captured, so no throughput claim is made**. Ghidra analysis itself took 4 s on the synthetic fixture |
| POC 2 (angr / Capstone / PyVEX) | angr 10.0.5, CFGFast, MIPS32 LE auto-detected; Capstone 5.0.9; Python venvs under `poc2/` (not deliverables) | Fixture-scale only; no timing claims beyond counts |
| POC 3 (build / codegen) | Ubuntu 24.04, GCC 13.3, CMake 3.28.3, Make, 4 cores / 7.7 GiB; main clone Release | **All wall times were measured while a separate Phase 15 workload shared the 4 cores — order-of-magnitude, not clean-room numbers.** Do not quote them as clean-room results |
| Category D "measurements" | None built or run in Category D (no Vulkan GPU assumed) | D's figures below are cited from prior in-tree measurements or other categories, flagged as such |

Raw outputs live in the research scratch area (`~/workspace/zmbor-research/poc1/results/`,
`poc2/*.json`, `poc3/results/`) and in the prototype patches (see §5). The large local
installs (`poc1/ghidra/`, `poc1/downloads/`, `poc2/.venv-*/`) are **not** deliverables and
are excluded from the patches.

## 1. POC 1 — Ghidra headless + Emotion Engine Reloaded: RAN, PASSED

**Commands (full procedure in `poc1/README.md`):** build the EE extension manually
(Ghidra's own `support/sleigh` → `r5900.sla`, `javac` the 15 Java sources →
`EmotionEngine.jar`, ship `.slaspec` + all `.sinc` alongside the `.sla` — Ghidra fails at
load if the `.slaspec` is absent), then `run_poc1.sh` (headless export via
`ExportPS2FunctionsHeadless.java`, TESTED), then score with `validate_poc1.py` under Zmbor's
promotion rules (executable-section, alignment, range, precedence, CFG validation).

**Setup notes (failures encountered, honestly recorded):** Gradle failed three times in the
sandbox ("Could not receive a message from the daemon" — daemon starts, client cannot talk
to it, no error in the daemon log); the manual path above is the reproducible workaround.
Two POC bugs found and fixed: `run_poc1.sh` needed `chmod +x`; the headless script used a
`printer` field that does not exist in Ghidra 12's `GhidraScript` (fixed to `println`).
Ghidra 12.1.4's local SHA-256 is recorded in the POC 1 README but was **not** verified
against an upstream hash — none was published on the fetched release page.

**Results (scored by `validate_poc1.py`):**

| Fixture | Reported | True positives | False positives | Missed |
|---|---|---|---|---|
| synthetic stripped (ground truth) | 8 | 8/8 | 0 | 0 |
| basic (symtab) | 7 | 7/7 | 0 | 0 |
| branchdelay (symtab) | 13 | 13/13 | 0 | 0 |
| vu_macro (symtab, COP2-heavy) | 7 | 7/7 | 0 | 0 |
| gs_sprite (symtab) | 4 | 4/4 | 0 | 0 |

**What the numbers do and do not mean:**

- Ghidra's synthetic CSV names `entry` + `FUN_00100028/44/b4/dc/e8/114/120`; `f_switch`
  spans its switch blocks internally (not reported as functions); neither switch blocks nor
  the bare leaf are reported.
- Ghidra reported `dead_func` (unreferenced): its list is a **boundary list, not a
  proven-reachable set** — no tool here delivers the reachable set.
- The four e2e fixtures carry symbols, so those rows measure **agreement-with-symbols, not
  stripped recovery**; only the synthetic row measures stripped recovery.
- The synthetic fixture is small and MIPS-subset-only (no MMI, no COP2, no relocations, no
  overlays, no C++/vtables, no PIC, no real-game shapes) — passing it is necessary, not
  sufficient.
- Ghidra's exported ends use body-max+1 and may understate spans for functions with gaps
  (branchdelay `bd_*` rows report Size 12); starts (the scored quantity) are unaffected;
  **end-exactness is NOT demonstrated**.
- The export is functions-only: the in-tree script's label / materialised-address /
  data-pointer collection is NOT ported. Port it and re-measure before calling the pipeline
  complete — it can only add candidates and may add false positives.
- `loadGhidraMap` execution was **NOT run** (format-checked only); loading a POC 1 CSV
  through it is the ADOPT completion condition, not a completed step.
- Ghidra analysis itself: 4 s on synthetic. No per-fixture throughput claim (see §0).

**Raw outputs:** `poc1/results/*.ghidra.csv` (5 CSVs, in the POC 1 patch);
`poc1/fixtures/` (ELFs + `gen_synthetic.py` + `synthetic_stripped.truth.json`).

## 2. POC 2 — angr / Capstone / PyVEX probes: RAN, MEASURED, STAYS AT STUDY

**Commands (full procedure in `poc2/README.md`):** `run_poc2.sh` → `run_angr.py` (CFGFast),
`pyvex_ee_probe.py`, `capstone_probe.py`. angr 10.0.5, MIPS32 LE auto-detected.

**angr CFGFast results (same fixtures as POC 1):**

| Fixture | Reported | True positives | Extras (false positives for promotion purposes) |
|---|---|---|---|
| synthetic stripped | 12 | 8/8 | 4 (three switch blocks + bare leaf) |
| basic | 10 | 7/7 | 3 |
| vu_macro | 9 | 7/7 | 2 |

Perfect recall, failing precision — angr does not beat the fallback scan in the sense that
matters, since promotion must be exact. Attribution caveat: CFGFast's mechanism for finding
`f_target`/`f_callback` was not isolated, so **"angr resolves JALR-constant/data-pointer
edges" is UNPROVEN** — do not cite it as a finding.

**PyVEX EE probe (the decisive result):** COP2 and LQ words lift as `Ijk_NoDecode`, SQ as
`Ijk_SigILL`, and MMI as a **silent no-op** (bare IMark, `Ijk_Boring`, no data flow). Any
angr result on real EE code inherits this silent damage. This alone keeps angr at STUDY:
no VEX EE lifter exists that preserves EE semantics, and no precision fix exists for the
switch-block/bare-leaf extras.

**Capstone 5.0.9 probe:** LQ→`xori.b`, SQ→`ext`/no-decode, MMI→`madd`/no-decode,
COP2→no-decode; **13/227 vu_macro `.text` words fail (5.7%), clustered exactly on EE
instructions.** Capstone is a decoder probe only, never an analyzer for this target.

**Raw outputs:** `poc2/results_synthetic.json`, `results_basic.json`,
`results_vu_macro.json`, `capstone_probe_results.json`, `pyvex_ee_probe_results.json`
(all in the POC 2 patch). The CCC standalone converter and Rizin were NOT run.

## 3. POC 3 — Compile speed / profiling baseline: RAN, MEASURED

**Commands (full procedure in `poc3/README.md`):** `measure_baseline.sh` (configure, full
build, incremental rebuild, generation timing, binary sizes, suite runs) and
`ccache_experiment.sh` (cold vs warm on the `ps2_recomp` target). Build dir:
`~/workspace/zmbor-research/main-build/` (own directory; never the COP0 coordinator's
build dirs). Dependency cache reused read-only via `FETCHCONTENT_SOURCE_DIR_*` overrides;
configure flags `-DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF
-DPS2X_ENABLE_DEBUG_UI=OFF`, `-G "Unix Makefiles"`. sccache reported "not found" (expected
— the hook exists, the binary does not).

**Baseline (main clone, Release, GCC 13.3, CMake 3.28.3, Make, 4 cores / 7.7 GiB, shared
with Phase 15 — order-of-magnitude only):**

| Quantity | Measured |
|---|---|
| Configure (CMake) | 44.9 s |
| Full build | **874.8 s**, rc 0, build dir 303 MB |
| Incremental: one recompiler TU (`function_emitter.cpp`) + relink of `ps2_recomp` | 53.7 s |
| Rebuilt `ps2_recomp` cascading into 4 e2e regenerations + 8 TU compiles + 4 LTO links | 244.5 s (a measured "no-op" that was not a no-op — a finding in itself) |
| Peak build memory | **UNKNOWN (isolated)** — no `/usr/bin/time` here; the sampler's 2,490,680 KB figure is system-wide compiler RSS including the concurrent Phase 15 build and must not be quoted as Zmbor's peak |
| Generation time (`ps2_recomp`, externally timed — it prints no timing) | basic 0.101 s, branchdelay 0.091 s, vu_macro 0.036 s, gs_sprite 0.044 s — generation is negligible; compiling + LTO-linking generated code is where time goes |
| Binaries | `ps2_recomp` 2,217,736 B; e2e 3.26–3.28 MB each; `ps2x_tests` 8,315,824 B; generated sources 11–51 KB; guest ELFs 5,364–6,080 B |
| Suites run in the clone | `ps2x_tests` binary (not a CTest test in main) **620/620** (56.8 s); full CTest **54 passed / 1 skipped (z3, solver absent) / 0 failed** (38.0 s); e2e **4/4**; IOP standalone suite (separate configure, option OFF by default in main) **4/4** (build 60.2 s, tests 0.31 s) |
| Suites NOT run here | ps2autotests ladder; ASan+UBSan builds |
| Direct e2e runs (all rc 0, expected lines matched) | basic 1.32 s, branchdelay 1.30 s, vu_macro 1.70 s, gs_sprite 6.79 s — dominated by runtime startup/frame loop, not guest CPU; **not CPU benchmarks** |

**ccache experiment (target `ps2_recomp`, 117 compilations, ccache 4.9.1 installed via
apt for this experiment):** cold 134.0 s (0 hits) vs warm 22.8 s (117 hits) = **5.9× on that
subset (not a full-build claim)**. sccache itself was NOT run; the ccache result is a proxy
for the sccache adoption step (install binary + measure).

**TU compile experiment:** a 51 KB generated TU costs ~6 s even at -O0 (repeat -O0 6.03 s
vs -O3 6.11 s) — header parsing, not -O level, dominates.

**PCH prototype (isolated, no clone changes):** PCH build 44.2 s / 129 MB; TU with PCH
3.31 s vs ~6.1 s without (use verified via `-H` `! pch.h.gch`); break-even ≈ 16 TUs per PCH
rebuild; object size identical.

**Production flags confirmed (not "to add"):** generated-TU flags are `-O3 -flto=auto
-fno-fat-lto-objects -ffp-contract=off -msse4.1` — full LTO is already on in Release, so
"add LTO" is not a valid recommendation; the open question is ThinLTO vs this baseline.
`-ffast-math` is policy-forbidden for arithmetic-header code (fpenv negative test, passed).
e2e uses single-file output (one TU per program); the recompiler's multi-file mode exists
but is unexercised by e2e.

**Raw outputs:** `poc3/results.md` (digest) and `poc3/results/` logs (build summary,
ccache cold/warm logs and stats) — summaries in the POC 3 patch; full logs remain in the
research scratch area.

## 4. Graphics numbers — provenance, not a benchmark table

Category D built and ran nothing (no Vulkan GPU assumed). The following figures are
**cited, not measured in this program**, and must not be quoted as Zmbor measurements:

| Figure | Source | Status |
|---|---|---|
| `gs_sprite` 6.79 s direct run | POC 3, measured (see §3) | Measured, but dominated by runtime startup/frame loop, not guest CPU or GS work |
| `gs_sprite` 5.16–5.61 s (Category D's citation) | Category D research, from prior in-tree measurements | **Provenance not re-verified in this program** — D's figure and POC 3's 6.79 s differ because they are different runs on a shared, loaded machine; neither is a GS benchmark |
| `gs_sprite`-equivalent ~8.0 ms GS-only (~120–125 fps); 1,000 flat 32×32 sprites ~27.6–33.1 ms; 1,000 flat triangles ~33.4–34.6 ms; present ~2.1 ms floor | Prior GS backend assessment (standalone harness, VM AMD EPYC 9D25 4 vCPUs headless) | Prior measurement, not re-run here; GS-only time, not frame time |
| VU ~50× slower than real VU1 (scheduler bookkeeping) | Category D research | Cited, not re-measured here; a VU/scheduler problem, not a GS problem |
| paraLLEl-GS: 2.8 ms CPU + 2.9 ms GPU vs GL 5.5 + 7.5 ms (9800X3D + RTX 5080) | BT3's numbers, cited by Category D | **BT3's, not Zmbor's, and do not transfer** |
| ps2-web-recomp: dispatch batching 650→80/frame, 16 MB upload cache (~60% less per-frame data) | Project-reported, cited by Category C | UNVERIFIED (project-reported, no run here) |
| TimeSplitters staged 5→60 fps table | Project-reported, cited by Category C | UNVERIFIED (project-reported, fixed-seed route, no run here) |

**Conclusion from §4:** there is no scene-at-speed GS benchmark in Zmbor today. The
fixture harness (Category D rank 0) is the prerequisite for any future GS number.

## 5. Unmeasured claims — labelled as such

- **Category F magnitudes:** no experiment in Category F was run. Every benefit magnitude
  there is a prior with a falsifier, not a measurement.
- **Category B experiments (E1–E8):** nothing run. All B benefits are design judgements
  awaiting their experiments.
- **Category C playability/fps claims:** project- or aggregator-reported, UNVERIFIED. No
  game was run. The Fixer diff was inspected at document/metadata level, not hunk-by-hunk.
- **Clang, ThinLTO, Tracy, Benchmark, sccache, mold, lld on Zmbor:** NOT run here (not
  installed, or needing a workload that does not exist). Their verdicts rest on
  documentation, Zmbor's prior evaluations, and the ccache proxy measurement.
- **Peak build memory:** UNKNOWN (see §3). **Per-fixture Ghidra throughput:** not captured
  (see §0). **ps2autotests ladder re-run:** not performed (baseline is historical).
- **No real-game runtime performance is known, and none is claimed.** The 4-program
  micro-corpus does not predict game-scale generation, compile, link, memory, or runtime
  behavior; all game-scale statements in this program are experiment designs, not findings.

## 6. Prototype patches (what is in each, and how to verify it)

| Patch (in `~/workspace/your_files/`) | Contents | Verify by |
|---|---|---|
| `zmbor-research-poc1-ghidra-headless.patch` | `run_poc1.sh`, `ExportPS2FunctionsHeadless.java`, `validate_poc1.py`, `README.md`, `fixtures/` (ELFs, `gen_synthetic.py`, truth JSON), `results/*.ghidra.csv` | Follow `README.md`: build the EE extension manually, run the headless export, score with `validate_poc1.py`; expect the §1 table on the fixtures |
| `zmbor-research-poc2-angr-oracle.patch` | `run_poc2.sh`, `run_angr.py`, `pyvex_ee_probe.py`, `capstone_probe.py`, `README.md`, result JSONs | Follow `README.md`: run the angr/Capstone/PyVEX probes; expect the §2 tables (angr extras, VEX `NoDecode`/silent-no-op, Capstone 5.7% EE-word failure) |
| `zmbor-research-poc3-compile-speed.patch` | `measure_baseline.sh`, `ccache_experiment.sh`, `README.md`, `results.md`, summary logs | Follow `README.md`: run the baseline + ccache scripts in a fresh clone; expect §3-scale numbers (order-of-magnitude on a shared machine, not bit-identical timings) |

Patches are new-file additions under `research/poc1/`, `research/poc2/`, `research/poc3/`
respectively, applicable with `git apply` at a repo root. They are **not** applied to any
checkout by this program. Large local installs (`poc1/ghidra/`, `poc1/downloads/`,
`poc2/.venv-*/`, full `poc3/results/` logs) are excluded; the READMEs state how to
reproduce them. Integration instructions (feature flags, completion conditions, what is
deliberately not included) are in the READMEs, the feasibility matrix §3, and the design
document.
