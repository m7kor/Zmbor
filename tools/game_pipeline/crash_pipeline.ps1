# Reproduces the Crash Twinsanity (SLUS_209.09) pipeline. Private: lives under work/ (git-ignored).
# Stages: analyze -> recompile -> native build (ps2x_game) -> run (bounded, hidden window, logs to diagnostics/).
#   -Tag <name>        prefix for this run's logs/frame/summary (default: run)
#   -BuildDir <dir>    build tree holding ps2xRuntime/ps2x_game.exe (default: build)
#   -Trace <list>      comma list exported as ZMBOR_TRACE_<X>=1 (e.g. stubs,io)
#   -Disc <dir>        disc working directory (default: work/crash_twinsanity/disc)
param([int]$RunSeconds = 60, [switch]$SkipRecompile, [switch]$SkipBuild, [string]$Tag = 'run',
      [string]$BuildDir = 'build', [string]$Trace = '', [string]$Disc = '', [int]$SeriesMs = 0,
      [string]$Sampler = '', [int]$ProfileSeconds = 60, [int]$ProfileDelaySeconds = 0)
. D:\Project\Testing\work\scripts\env.ps1
$w = 'D:\Project\Testing\work\crash_twinsanity'
if ($Tag -notmatch '^[A-Za-z0-9_-]+$') { throw 'Tag must contain only letters, digits, underscores or hyphens' }
if ($RunSeconds -le 0 -or $ProfileSeconds -le 0 -or $ProfileDelaySeconds -lt 0) { throw 'Durations must be positive; profile delay may be zero' }
$samplerExe = if ($Sampler) { (Resolve-Path -LiteralPath $Sampler -ErrorAction Stop).Path } else { '' }
# A tag identifies immutable evidence. Never erase an earlier run to reuse it.
if ((Get-ChildItem "$w\diagnostics\$Tag.*", "$w\diagnostics\native_build_$Tag.log", "$w\screenshots\$Tag.*", "$w\screenshots\${Tag}_series" -ErrorAction SilentlyContinue)) {
    throw "Evidence already exists for tag '$Tag'; choose a new tag"
}
if (-not $Disc) { $Disc = "$w\disc" }
$elf = "$w\analysis\SLUS_209.09"
if (-not $SkipRecompile) {
    & .\build\ps2xAnalyzer\ps2_analyzer.exe $elf "$w\analysis\$Tag.toml" *> "$w\diagnostics\$Tag.analyzer.log"
    if ($LASTEXITCODE) { throw "ps2_analyzer failed" }
    (Get-Content "$w\analysis\$Tag.toml") -replace '^output = .*', 'output = "D:/Project/Testing/work/crash_twinsanity/generated/"' |
        Set-Content "$w\analysis\$Tag.toml"
    # The conservative SCE scanner requires 256 static bits. This SDK 3.x
    # 16-byte public leaf is below that threshold; bind only its verified hash.
    python -I D:\Project\Testing\tools\game_pipeline\bind_sdk_stub.py $elf "$w\analysis\$Tag.toml" `
        --name sceMpegGetPictureAbort --address 0x002B7CA0 --size 16 --sha1 929d5c4ab5abc2bff17c24561aafa4e9e7070ad1
    if ($LASTEXITCODE) { throw 'SDK abort leaf binding failed' }
    & .\build\ps2xRecomp\ps2_recomp.exe "$w\analysis\$Tag.toml" *> "$w\diagnostics\$Tag.recomp.log"
    if ($LASTEXITCODE) { throw "ps2_recomp failed" }
}
if (-not $SkipBuild) {
    cmake --build $BuildDir --target ps2x_game --parallel 8 *> "$w\diagnostics\native_build_$Tag.log"
    if ($LASTEXITCODE) { throw "native build failed" }
}
Get-ChildItem env: | Where-Object Name -like 'ZMBOR_TRACE_*' | ForEach-Object { Remove-Item "env:$($_.Name)" }
foreach ($t in ($Trace -split ',' | Where-Object { $_ })) { Set-Item "env:ZMBOR_TRACE_$($t.ToUpper())" '1' }
$env:PS2X_HIDDEN_WINDOW = '1'
$env:PS2X_TTY_LOG = "$w\diagnostics\$Tag.tty.log"
New-Item -ItemType Directory -Force "$w\screenshots" | Out-Null
$env:PS2X_DUMP_FRAME = "$w\screenshots\$Tag.frame.ppm"
$env:ZMBOR_SUMMARY_FILE = "$w\diagnostics\$Tag.summary.txt"
# The runtime stops itself at RunSeconds (exit 124 = harness timeout) so shutdown and the summary still run;
# the outer limit below only catches a hung process.
$env:ZMBOR_RUN_SECONDS = "$RunSeconds"
Remove-Item env:ZMBOR_FRAME_SERIES -ErrorAction SilentlyContinue
if ($SeriesMs -gt 0) {
    $series = "$w\screenshots\${Tag}_series"
    New-Item -ItemType Directory -Force $series | Out-Null
    $env:ZMBOR_FRAME_SERIES = $series
    $env:ZMBOR_FRAME_SERIES_MS = "$SeriesMs"
}
$exe = Resolve-Path "$BuildDir\ps2xRuntime\ps2x_game.exe"
$sourceHead = (git rev-parse HEAD).Trim()
$sourceDirty = [bool](git status --porcelain)
$exeHash = (Get-FileHash -LiteralPath $exe.Path -Algorithm SHA256).Hash
# Legacy aggressive function tracing writes a fixed filename in the run directory.
# Archive both an older log and this run's log instead of letting the next run truncate it.
$functionLog = Join-Path $Disc 'ps2_log.txt'
if (Test-Path -LiteralPath $functionLog) {
    Move-Item -LiteralPath $functionLog -Destination "$w\diagnostics\$Tag.previous-function-log.txt"
}
$start = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList "`"$Disc\SLUS_209.09`"" `
    -WorkingDirectory $Disc -RedirectStandardOutput "$w\diagnostics\$Tag.stdout.log" `
    -RedirectStandardError "$w\diagnostics\$Tag.stderr.log" -PassThru -WindowStyle Hidden
$h = $p.Handle  # keep the handle so ExitCode stays readable
$profiler = $null
if ($Sampler) {
    $profiler = Start-Process -FilePath $samplerExe `
        -ArgumentList @($p.Id, $ProfileSeconds, "`"$w\diagnostics\$Tag.profile`"", 10, $ProfileDelaySeconds) `
        -RedirectStandardOutput "$w\diagnostics\$Tag.profile.stdout.log" `
        -RedirectStandardError "$w\diagnostics\$Tag.profile.stderr.log" -WindowStyle Hidden -PassThru
    $profileHandle = $profiler.Handle
}
$peak = 0; $cpu = 0
while (-not $p.HasExited -and ((Get-Date) - $start).TotalSeconds -lt ($RunSeconds + 30)) {
    Start-Sleep -Milliseconds 500
    try { $p.Refresh(); $peak = [math]::Max($peak, $p.PeakWorkingSet64); $cpu = $p.TotalProcessorTime.TotalSeconds } catch {}
}
$outcome = 'exited'
if (-not $p.HasExited) {
    # Deliberate harness timeout: this is not a guest result.
    try { $p.Refresh(); $cpu = $p.TotalProcessorTime.TotalSeconds; $peak = [math]::Max($peak, $p.PeakWorkingSet64) } catch {}
    Stop-Process -Id $p.Id -Force; $p.WaitForExit(); $outcome = 'hung_killed'
}
$term = Select-String -Path "$w\diagnostics\$Tag.stderr.log" -Pattern '\[zmbor:termination\].*' -ErrorAction SilentlyContinue |
    Select-Object -Last 1 | ForEach-Object { $_.Matches[0].Value }
$wall = ((Get-Date) - $start).TotalSeconds
if (Test-Path -LiteralPath $functionLog) {
    Move-Item -LiteralPath $functionLog -Destination "$w\diagnostics\$Tag.function-log.txt"
}
if ($profiler -and -not $profiler.WaitForExit(10000)) {
    Stop-Process -Id $profiler.Id -Force
    $profiler.WaitForExit()
}
$out = Get-Content "$w\diagnostics\$Tag.stdout.log" -Raw -ErrorAction SilentlyContinue
$frames = if ($out) { ([regex]::Matches($out, '\[frame:upload\]')).Count } else { 0 }
$frameStats = if (Test-Path $env:PS2X_DUMP_FRAME) { python -I D:\Project\Testing\work\scripts\ppm2png.py $env:PS2X_DUMP_FRAME "$w\screenshots\$Tag.frame.png" } else { 'no frame' }
$summary = [ordered]@{
    tag = $Tag; outcome = $outcome; exit_code = $(if ($outcome -eq 'exited') { $p.ExitCode } else { $null })
    wall_s = [math]::Round($wall, 1); cpu_s = [math]::Round($cpu, 1); peak_ws_mb = [math]::Round($peak / 1MB)
    frame_uploads_logged = $frames; last_frame = "$frameStats"; build = $BuildDir; trace = $Trace; termination = "$term"
    profiler_exit = $(if ($profiler) { $profiler.ExitCode } else { $null })
    source_head = $sourceHead; source_dirty = $sourceDirty; executable_sha256 = $exeHash
}
$summary | ConvertTo-Json | Set-Content "$w\diagnostics\$Tag.json"
$summary | ConvertTo-Json
