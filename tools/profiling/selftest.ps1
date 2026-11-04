param(
    [Parameter(Mandatory=$true)][string]$Sampler,
    [Parameter(Mandatory=$true)][string]$OutputPrefix
)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Sampler).Path
# A fresh prefix preserves older measurements. The worker terminates itself.
if (Test-Path -LiteralPath "$OutputPrefix.meta.txt") { throw 'Choose a new output prefix' }
$worker = Start-Process -FilePath $exe -ArgumentList '--workload','8' -WindowStyle Hidden -PassThru
try {
    & $exe $worker.Id 4 $OutputPrefix 10
    if ($LASTEXITCODE) { throw "Sampler failed: $LASTEXITCODE" }
    $functions = Import-Csv -LiteralPath "$OutputPrefix.functions.csv"
    $leaf = @($functions | Where-Object { $_.function -like '*profileBusyLeaf*' })
    $parent = @($functions | Where-Object { $_.function -like '*profileBusyParent*' })
    if (-not $leaf -or [int]$leaf[0].wall_self_samples -le 0) { throw 'No known hot leaf resolved' }
    if (-not $parent -or [int]$parent[0].wall_inclusive_samples -lt [int]$leaf[0].wall_self_samples) {
        throw 'Known parent stack was not unwound'
    }
    $threads = Import-Csv -LiteralPath "$OutputPrefix.threads.csv"
    if (-not @($threads | Where-Object { [double]$_.observed_cpu_seconds -gt 0 })) { throw 'No thread CPU time recorded' }
    $meta = Get-Content -LiteralPath "$OutputPrefix.meta.txt" -Raw
    if ($meta -notmatch 'not CPU-cost attribution') { throw 'Wall-time interpretation caveat missing' }
    $suffixes = @('.functions.csv','.stacks.csv','.threads.csv','.meta.txt')
    $hashes = @{}
    foreach ($suffix in $suffixes) { $hashes[$suffix] = (Get-FileHash -LiteralPath "$OutputPrefix$suffix").Hash }
    & $exe $worker.Id 1 $OutputPrefix 10
    if ($LASTEXITCODE -ne 5) { throw 'Output collision was not refused' }
    foreach ($suffix in $suffixes) {
        if ((Get-FileHash -LiteralPath "$OutputPrefix$suffix").Hash -ne $hashes[$suffix]) {
            throw "Output collision changed $suffix"
        }
    }
    $global:LASTEXITCODE = 0
    Write-Output 'PASS: hot leaf, inclusive parent, thread CPU time, wall-time caveat, and collision preservation'
} finally {
    $worker.WaitForExit()
    $worker.Dispose()
}
