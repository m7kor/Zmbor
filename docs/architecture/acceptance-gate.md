# Acceptance gate and result identity

`tools/acceptance/gate.ps1` runs the full local acceptance gate and writes a manifest that ties every number to the
code that produced it. Windows/MSVC; run from the repository root inside a `vcvars64` environment after configuring
the build directory with `-DPS2X_HWTEST_GENERATED=<prepared dir>`.

```
pwsh -File tools/acceptance/gate.ps1 -Name p6_candidate -BuildDir build `
     -HwGenerated D:\Project\Zmbor\hwtest_work\gen6 `
     -AutotestsDir D:\Project\Zmbor\thirdparty-data\ps2autotests `
     -ReportDir D:\Project\Zmbor\hwtest_work\p6 -BaselineReport D:\Project\Zmbor\hwtest_work\p6\p6_baseline.json `
     -Python C:\path\to\python.exe
```

Steps: build `ps2_recomp` -> `hwtest.py prepare` with **that** recompiler (all programs regenerated; `-SkipPrepare`
only when the recompiler is unchanged) -> full build -> `ctest -L hwtest` -> `hwtest.py report` against the baseline
-> `ps2x_tests` -> `ctest -LE hwtest` and `ctest -L e2e` -> `ps2xIOP` ctest.

`<Name>.manifest.json` records: source commit and whether the tree was clean, compiler banner, the relevant CMake
cache entries, per-step durations, build errors, the hardware summary with regressions and failing programs, unit
and ctest results, and SHA-256 of `ps2_recomp`, `ps2_runtime` and every `ps2x_*` program executable. A result whose
manifest shows `tree_clean: false` or a different commit is not a result for that commit.

Phase 6 baseline (`p6_baseline.manifest.json`, commit `0bac702`, clean): 55/55 executed, 53 exact, 8202/8250 lines,
0 crashes, 0 timeouts, 0 regressions against Phase 5's final report; `ps2x_tests` 541/541; ctest 52/52; e2e 4/4;
ps2xIOP 4/4; 1010 s.
