param(
    [Parameter(Mandatory)] [string]$Name,                         # report name, e.g. p6_baseline
    [string]$BuildDir = 'build',
    [Parameter(Mandatory)] [string]$HwGenerated,                  # hwtest work dir (prepared programs)
    [Parameter(Mandatory)] [string]$AutotestsDir,                 # ps2autotests checkout (tests/ inside)
    [Parameter(Mandatory)] [string]$ReportDir,                    # where <Name>.json/.md/.manifest.json go
    [string]$BaselineReport = '',                                 # previous hwtest report json for regressions
    [string]$Python = 'python',
    [switch]$SkipPrepare,                                         # reuse prepared programs (only if recompiler unchanged)
    [switch]$SkipHardware
)
# Zmbor acceptance gate (docs/architecture/acceptance-gate.md). Run from the repository root inside an MSVC developer
# environment (vcvars64) with cmake/ninja on PATH, after configuring $BuildDir with -DPS2X_HWTEST_GENERATED=$HwGenerated.
# Steps: identity check -> build ps2_recomp -> hwtest prepare (regenerates all programs with THIS recompiler) -> full
# build -> hardware matrix -> report vs baseline -> ps2x_tests -> ctest (non-hwtest, e2e) -> ps2xIOP ctest -> manifest.
# The manifest records the source commit, tree cleanliness, compiler, configuration and SHA-256 of the binaries, so a
# result can be tied to the exact code that produced it.
$ErrorActionPreference = 'Continue'
$root = (Get-Location).Path
$sw = [Diagnostics.Stopwatch]::StartNew()
$m = [ordered]@{ schema = 1; name = $Name; started = (Get-Date).ToString('o') }
$m.commit = (git rev-parse HEAD).Trim()
$m.tree_clean = [string]::IsNullOrEmpty((git status --porcelain | Out-String).Trim())
$m.compiler = ((cmd /c "cl 2>&1") | Select-Object -First 1).Trim()
$cache = Join-Path $BuildDir 'CMakeCache.txt'
$m.configuration = [ordered]@{}
foreach ($line in (Get-Content $cache | Select-String '^(CMAKE_BUILD_TYPE|CMAKE_GENERATOR|PS2X_HWTEST_GENERATED|PS2X_HWTEST_OPTIMIZE|PS2X_BUILD_STUDIO|PS2X_ENABLE_FFMPEG|PS2X_IOP_BUILD_TESTS):')) {
    $k, $v = $line.Line -split '=', 2; $m.configuration[$k.Split(':')[0]] = $v
}
function Step([string]$label, [scriptblock]$body) {
    $t = [Diagnostics.Stopwatch]::StartNew(); $out = & $body; $script:m["${label}_seconds"] = [int]$t.Elapsed.TotalSeconds; $out
}
Step 'build_recompiler' { cmake --build $BuildDir --target ps2_recomp 2>&1 | Select-String 'error' | Select-Object -First 3 } | ForEach-Object { "build: $_" }
if (-not $SkipHardware -and -not $SkipPrepare) {
    Step 'prepare' { & $Python -I tools/hwtest/hwtest.py prepare --tests (Join-Path $AutotestsDir 'tests') --recompiler (Join-Path $BuildDir 'ps2xRecomp/ps2_recomp.exe') --out $HwGenerated 2>&1 | Select-Object -Last 1 } | ForEach-Object { "prepare: $_" }
}
$m.prepared_with_this_recompiler = (-not $SkipHardware -and -not $SkipPrepare)
$buildErrors = Step 'build' { cmake --build $BuildDir --parallel 10 2>&1 | Select-String 'error C|error LNK|FAILED:' | Select-Object -First 10 | ForEach-Object Line }
$m.build_errors = @($buildErrors)
if (-not $SkipHardware) {
    Push-Location (Join-Path $BuildDir 'ps2xTest'); Step 'hwtest' { ctest -L hwtest -j 8 2>&1 | Out-Null }; Pop-Location
    $reportArgs = @('-I', 'tools/hwtest/hwtest.py', 'report', '--out', $HwGenerated, '--json', (Join-Path $ReportDir "$Name.json"), '--md', (Join-Path $ReportDir "$Name.md"))
    if ($BaselineReport) { $reportArgs += @('--baseline', $BaselineReport) }
    & $Python @reportArgs 2>&1 | Out-Null
    $r = Get-Content (Join-Path $ReportDir "$Name.json") -Raw | ConvertFrom-Json
    $m.hardware = [ordered]@{ summary = $r.summary; regressions = $r.regressions
        failing = @($r.tests | Where-Object { $_.status -ne 'PASS' } | ForEach-Object { "$($_.id): $($_.status) $($_.matching_lines)/$($_.expected_lines)" }) }
}
$unit = Step 'unit' { & (Join-Path $BuildDir 'ps2xTest/ps2x_tests.exe') 2>&1 }
$m.unit = (($unit | Select-String '^Passed:|^Failed:' | ForEach-Object Line) -join ' ')
$m.unit_failures = @($unit | Select-String '^\s+- ' | Select-Object -First 20 | ForEach-Object { $_.Line.Trim() })
Push-Location (Join-Path $BuildDir 'ps2xTest')
$m.ctest_non_hwtest = ((Step 'ctest' { ctest -LE hwtest -j 8 2>&1 } | Select-String 'tests passed|tests failed|\(Failed\)' | ForEach-Object Line) -join ' ')
$m.ctest_e2e = ((ctest -L e2e 2>&1 | Select-String 'tests passed|tests failed|\(Failed\)' | ForEach-Object Line) -join ' ')
Pop-Location
Push-Location (Join-Path $BuildDir 'ps2xIOP')
$m.iop = ((ctest -j 4 2>&1 | Select-String 'tests passed|tests failed' | ForEach-Object Line) -join ' ')
Pop-Location
$m.binaries = [ordered]@{}
foreach ($b in 'ps2xRecomp/ps2_recomp.exe', 'ps2xRuntime/ps2_runtime.lib', 'ps2xTest/ps2x_tests.exe') {
    $f = Join-Path $BuildDir $b
    if (Test-Path $f) { $m.binaries[$b] = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower() }
}
$hw = @(Get-ChildItem (Join-Path $BuildDir 'ps2xTest/e2e') -Filter 'ps2x_*.exe' -ErrorAction SilentlyContinue | Sort-Object Name)
$m.program_executables = [ordered]@{}
foreach ($e in $hw) { $m.program_executables[$e.Name] = (Get-FileHash $e.FullName -Algorithm SHA256).Hash.ToLower() }
$m.total_seconds = [int]$sw.Elapsed.TotalSeconds
$m.finished = (Get-Date).ToString('o')
$manifest = Join-Path $ReportDir "$Name.manifest.json"
$m | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 $manifest
"commit $($m.commit) clean=$($m.tree_clean)"
if ($m.hardware) { "hardware: $($m.hardware.summary.matching)/$($m.hardware.summary.discovered) exact, $($m.hardware.summary.matching_lines)/$($m.hardware.summary.expected_lines) lines, crash $($m.hardware.summary.crash), timeout $($m.hardware.summary.timeout), regressions $(@($m.hardware.regressions).Count)"; $m.hardware.failing }
"unit: $($m.unit)"; $m.unit_failures
"ctest: $($m.ctest_non_hwtest)"; "e2e: $($m.ctest_e2e)"; "iop: $($m.iop)"
"build errors: $(@($m.build_errors).Count)"; "manifest: $manifest"; "total sec: $($m.total_seconds)"
