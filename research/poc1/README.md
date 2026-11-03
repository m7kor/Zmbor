# POC 1 — Ghidra headless + Emotion Engine extension

## Status at delivery (2026-11-03)

- **Initial blocker (since removed):** this environment had no Java.
  `sudo apt-get install -y openjdk-21-jdk-headless` installed OpenJDK
  21.0.12.1, so the JDK blocker is GONE on this machine.
- **Ghidra:** downloaded and extracted in this session (12.1.4 PUBLIC,
  569,649,598 bytes, local SHA-256
  `ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db`
  — local hash only, not verified against an upstream-published hash).
  `ghidra/support/analyzeHeadless` prints its usage text under JDK 21.
- **EE extension: INSTALLED (manual build, 2026-11-03).** The Gradle
  build failed three times in this sandbox ("Could not receive a
  message from the daemon" — daemon starts, client cannot talk to
  it; no error in the daemon log). The extension was assembled
  manually instead and it works: (1) `support/sleigh r5900.slaspec`
  → `r5900.sla` (43,149 bytes; benign warning "8 NOP constructors
  found"); (2) `javac` the 15 Java sources against Ghidra's jars
  (clean) → `lib/EmotionEngine.jar` (103,116 bytes); (3) copy
  `data/languages/` (`.ldefs`, `.cspec`, `.pspec`, `.dwarf`,
  `.opinion`, `.sla`, AND `.slaspec` + all `.sinc` — Ghidra FAILS
  at load if the `.slaspec` is absent), `data/manuals/`,
  `data/patterns/`, `ghidra_scripts/`, `Module.manifest`, and
  `extension.properties` (version `2.1.39+head-4c787f8`, i.e.
  source HEAD, NOT the v2.1.38 release) into
  `ghidra/Ghidra/Extensions/ghidra-emotionengine-reloaded/`.
  Two POC bugs fixed along the way: `run_poc1.sh` needed `chmod +x`,
  and the headless script used a `printer` field that does not exist
  in Ghidra 12's `GhidraScript` (fixed to `println`).
- **POC 1 RESULTS (measured, CSVs in `results/`):** synthetic
  stripped 8/8 TP, 0 FP; basic 7/7; branchdelay 13/13; vu_macro 7/7;
  gs_sprite 4/4 — zero false positives on all five fixtures.
  The headless script is therefore TESTED (functions-only scope;
  see the section file §3 caveats).
- **Ghidra output HAS been produced:** the CSVs in `results/` are
  real headless runs; do not hand-edit them.

## Prerequisites

- JDK 21 on PATH (`java -version` → 21.x). Ghidra 12.1.x needs JDK 21;
  Ghidra 12.2 reportedly needs JDK 25 — do not mix versions.
- Ghidra extracted to `./ghidra` (or set `GHIDRA_HOME`).
- `ghidra-emotionengine-reloaded` installed in that Ghidra.
- Python 3 (for `validate_poc1.py`) and `readelf` (binutils).

## Exact commands

```bash
cd poc1
# 1. JDK (Ubuntu example; use Temurin zip on other hosts)
sudo apt-get install -y openjdk-21-jdk-headless
java -version

# 2. Ghidra 12.1.4 (verify size/hash against the release page before use)
curl -fLO https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip
unzip -q ghidra_12.1.4_PUBLIC_20260921.zip -d .
mv ghidra_12.1.4_PUBLIC ghidra   # or: GHIDRA_HOME=$PWD/ghidra_12.1.4_PUBLIC
ls ghidra/support/analyzeHeadless

# 3. EE extension: install the v2.1.38 release zip via the Ghidra GUI
#    (File > Install Extensions) once, then confirm the language exists
#    by importing one fixture in the GUI (processor r5900:LE:32:default).

# 4. Run (synthetic stripped fixture first, then the four main e2e ELFs)
./run_poc1.sh fixtures/synthetic_stripped.elf results
./run_poc1.sh fixtures/basic.elf results
./run_poc1.sh fixtures/branchdelay.elf results
./run_poc1.sh fixtures/vu_macro.elf results
./run_poc1.sh fixtures/gs_sprite.elf results
```

## Files

- `run_poc1.sh` — checks for `java`/`analyzeHeadless`, imports with
  `-processor r5900:LE:32:default`, runs the headless export, validates.
  Exits 2 on a missing JDK/launcher; never fabricates a CSV.
- `ExportPS2FunctionsHeadless.java` — NON-interactive variant of
  Zmbor's in-tree `ps2xRecomp/tools/ghidra/ExportPS2Functions.java`
  (the in-tree script calls `askFile` and cannot run headless).
  Functions-only by design; see its header for what it omits and why.
  UNTESTED until the first successful run above.
- `validate_poc1.py` — scores a CSV against ground truth (or symtab)
  with Zmbor's promotion rules: 4-aligned, inside an executable
  section, not a delay slot, not a known non-function.
- `fixtures/` — four main e2e ELFs regenerated from main's own
  `ps2xTest/e2e/e2e_programs.py`, plus `synthetic_stripped.elf`
  (stripped; verified with `readelf -s` → no symbols),
  `synthetic_stripped.truth.json`, and `gen_synthetic.py`
  (the generator; re-run it to regenerate the fixture deterministically).
- `downloads/` — local Ghidra zip (not a deliverable; large).
- `ghidra/` (when extracted) — local Ghidra install (not a deliverable).
- `ghidra_projects/` (created by runs) — Ghidra project databases.
- `results/` (created by runs) — `*.ghidra.csv` outputs.

## Scoring rules (what counts)

Only starts passing ALL promotion checks AND in the truth `functions`
set count as promotable. Reported switch blocks, the bare leaf, or
delay slots are false positives. Dead code (`dead_func`) reported by
Ghidra is dead-code reporting, not a proven edge. Ghidra guesses that
fail the checks are candidates, never edges.
