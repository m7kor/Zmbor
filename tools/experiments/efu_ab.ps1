param(
    [Parameter(Mandatory)] [ValidateSet('base', 'fmac_series', 'pi4_nearest', 'pi4_trunc24', 'pi4_double')] [string]$Variant,
    [string]$BuildDir = 'build-wip',
    [string]$HwGenerated = 'D:\Project\Zmbor\hwtest_work\gen5'
)
# EFU research A/B (docs/architecture/evidence-register.md 2.5). Production keeps the Phase 3 model ('base').
# Each variant patches ps2_vu1_lower.cpp in place, builds the vu_lower_efu hardware test and ps2x_tests, reports the
# matching lines and the EFU vector count, and ALWAYS restores the source. Results are selected on the same rows
# they are scored on: a better score here is a hypothesis, never evidence by itself.
#   fmac_series : series partial sums through ps2_vu_fmac::add (gap rule)            Phase 5 result: 182/223 (neutral)
#   pi4_nearest : fmac_series + pi/4 = 0x3F490FDB                                    Phase 5 result: 158/223 (falsified)
#   pi4_trunc24 : fmac_series + pi/4 = 0x3F490FDA                                    Phase 5 result: 183/223, vectors 168
#   pi4_double  : fmac_series + pi/4 as a double (0.78539816339744830962)             Phase 5 result: 183/223
# Run from the repository root inside a vcvars64 environment (cl, cmake on PATH).
$ErrorActionPreference = 'Stop'
$f = Join-Path (Get-Location) 'ps2xRuntime\src\lib\vu\ps2_vu1_lower.cpp'
$original = [IO.File]::ReadAllText($f)
$s = $original
function Sub([string]$old, [string]$new) {
    if (-not $script:s.Contains($old)) { throw "pattern missing: $old" }
    $script:s = $script:s.Replace($old, $new)
}
$constant = 'efuTrunc(t * t)) + 0.785398185253143);'
try {
    if ($Variant -ne 'base') {
        Sub '#include <limits>' "#include <limits>`r`n#include `"ps2_vu_fmac.h`""
        Sub '    double efuNearest(double d) { return efuValue(efuBits(d, true)); }' ("    double efuNearest(double d) { return efuValue(efuBits(d, true)); }`r`n" +
            "    double efuFmacAdd(double a, double b) { return efuValue(ps2_vu_fmac::add(efuBits(a, false), efuBits(b, false))); }")
        Sub 'accumulator = efuTrunc(accumulator + efuTrunc(c * power));' 'accumulator = efuFmacAdd(accumulator, efuTrunc(c * power));'
    }
    switch ($Variant) {
        'pi4_nearest' { Sub $constant 'efuTrunc(t * t)) + efuValue(0x3F490FDBu));' }
        'pi4_trunc24' { Sub $constant 'efuTrunc(t * t)) + efuValue(0x3F490FDAu));' }
        'pi4_double' { Sub $constant 'efuTrunc(t * t)) + 0.78539816339744830962);' }
    }
    [IO.File]::WriteAllText($f, $s)
    (Get-Item $f).LastWriteTime = Get-Date
    cmake --build $BuildDir --target ps2x_hw_vu_lower_efu ps2x_tests | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    Push-Location (Join-Path $BuildDir 'ps2xTest')
    $o = ctest -R '^hw_vu_lower_efu$' -V 2>&1
    Pop-Location
    "variant ${Variant}: " + (($o | Select-String 'lines match') | Select-Object -First 1).Line
    $u = & (Join-Path $BuildDir 'ps2xTest\ps2x_tests.exe') 2>&1
    ($u | Select-String 'EFU rows exact' | Select-Object -First 1).Line
    $copy = Join-Path $env:TEMP "zmbor_efu_$Variant.tty.txt"
    Copy-Item (Join-Path $HwGenerated 'vu_lower_efu\run.tty.txt') $copy
    "program output kept at $copy (compare with vu/lower/efu.expected)"
} finally {
    [IO.File]::WriteAllText($f, $original)
    (Get-Item $f).LastWriteTime = Get-Date
}
