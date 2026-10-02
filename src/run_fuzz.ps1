# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# Build and run the iSCSI wire fuzzer, twice.
#
# The interesting half is the SECOND run.  A fuzzer that has never found anything is
# indistinguishable from a fuzzer that cannot find anything, so this script also builds
# iscsi_fuzz.cpp with -DFUZZ_PROVE_DETECTION - one deliberate read past the end of a heap
# buffer - and REQUIRES AddressSanitizer to catch it.  If that build exits 0, this suite
# fails: the harness is what is under test, and a clean run from a blind harness is worse
# than no harness, because it looks like evidence.
#
# No adapter, no initiator, no NetworkDirect SDK, and a fixed iteration count, so it runs
# in CI on every push next to the other hardware-free suites.
#
#   .\run_fuzz.ps1                    # 200000 iterations
#   .\run_fuzz.ps1 -Iterations 2000000 -Seed 0x1234
param(
    [int]    $Iterations = 200000,
    [string] $Seed = "0x5EED1234"
)
$ErrorActionPreference = "Continue"
$src = $PSScriptRoot
$vs    = if ($env:ND_VS_DIR) { $env:ND_VS_DIR } else { "F:\Microsoft Visual Studio\18\Community" }
$vsdev = "$vs\Common7\Tools\VsDevCmd.bat"
$rc = 0

# ASAN on Windows needs its runtime DLL findable at run time; cl.exe does not copy it.
# Searched across VC rather than only VC\Tools\MSVC: the DLL ships with the toolset on
# some layouts and only in the redist on others, and CI's VS layout is not this machine's.
# A miss here FAILS the suite instead of quietly skipping the sanitizer, because a fuzz run
# without the sanitizer is a run that cannot see the thing it exists to see.
function Add-AsanToPath {
    $root = Join-Path $vs 'VC'
    $dll = Get-ChildItem $root -Recurse -Filter 'clang_rt.asan_dynamic-x86_64.dll' -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($dll) { $env:PATH = "$($dll.DirectoryName);$env:PATH"; return $true }
    return $false
}
if (-not (Add-AsanToPath)) {
    Write-Host "  the ASAN runtime DLL was not found under $vs - cannot judge a fuzz run"
    exit 1
}

function Build-Fuzzer([string] $exe, [string] $extra) {
    $obj = [System.IO.Path]::ChangeExtension($exe, '.obj')
    Remove-Item $exe, $obj -ErrorAction SilentlyContinue
    # /Zi is not decoration: without debug information MSVC answers a sanitizer build with
    # C5072, which /WX turns into an error - and an ASAN report with no file and no line
    # number is a report nobody can act on.
    $cmd = "call `"$vsdev`" -arch=x64 -no_logo >nul 2>&1 && cd /d `"$src`" && " +
           "cl /nologo /W4 /WX /std:c++17 /EHsc /Zi /fsanitize=address /I`"$src`" $extra " +
           "iscsi_fuzz.cpp /Fe:$([System.IO.Path]::GetFileName($exe)) " +
           "/Fo:$([System.IO.Path]::GetFileName($obj)) /link ws2_32.lib"
    $out = & cmd.exe /c $cmd 2>&1
    $buildRc = $LASTEXITCODE
    $out | Where-Object { $_ -match "error|warning" } | Select-Object -First 8 | ForEach-Object { "  $_" }
    # Judged by the compiler's exit code, never by "does the exe exist" (DESIGN 8.6).
    if ($buildRc -ne 0 -or -not (Test-Path $exe)) {
        Write-Host "  BUILD FAILED (rc=$buildRc) - refusing to run a stale binary"
        return $false
    }
    return $true
}

# --- 1. the real fuzzer: it must pass -----------------------------------------------
$good = "$src\iscsi_fuzz.exe"
if (-not (Build-Fuzzer $good "")) { exit 1 }
$out = & $good "$Iterations" "$Seed" 2>&1
$fuzzRc = $LASTEXITCODE
$out | ForEach-Object { "  $_" } | Select-Object -Last 6
if ($fuzzRc -ne 0) {
    Write-Host "  the fuzzer reported a failure - the seed and iteration above replay it"
    $rc = 1
}

# --- 2. the harness itself: it must FAIL -------------------------------------------
$bad = "$src\iscsi_fuzz_breach.exe"
if (-not (Build-Fuzzer $bad "/DFUZZ_PROVE_DETECTION")) { exit 1 }
$breachOut = & $bad "10" "0x1" 2>&1
$breachRc = $LASTEXITCODE
$caught = ($breachOut -join "`n") -match 'AddressSanitizer|heap-buffer-overflow|ERROR:'
if ($breachRc -eq 0 -or -not $caught) {
    Write-Host "  THE HARNESS IS BLIND: a deliberate read past the end of a heap buffer was"
    Write-Host "  not reported (rc=$breachRc).  A clean fuzz run proves nothing until this"
    Write-Host "  check passes - fix the sanitizer setup before trusting the run above."
    ($breachOut | Select-Object -First 6) | ForEach-Object { "    $_" }
    $rc = 1
} else {
    Write-Host "  proof of detection: ASAN caught the deliberate overflow (as required)"
}
Remove-Item "$src\iscsi_fuzz_breach.exe", "$src\iscsi_fuzz_breach.obj" -ErrorAction SilentlyContinue

if ($rc -eq 0) { Write-Host "wire fuzz: PASS (fuzzed clean, and the harness can still fail)" }
exit $rc
