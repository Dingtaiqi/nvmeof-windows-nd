# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# Build and run the wire-format self-test.
#
# It is built TWICE on purpose - once as C and once as C++ - because the file is
# included from both a .c and a .cpp translation unit in this project, and the
# byte layouts have to hold in both.  MSVC's C mode is where _Static_assert is
# not available, which is the whole reason NVMEOF_STATIC_ASSERT exists; building
# only the C++ side would never exercise that branch.
#
# /W4 /WX on both: a layout warning here is a wire-format bug, not a style nit.
#
# -Toolsets exists because "it builds here" is not "it builds there": this file is
# clean under MSVC 14.5x and was an error under 14.4x (C4127 on constant conditions,
# turned into C2220 by /WX) for 21 CI runs.  A toolchain difference is invisible until
# you compile with the other toolchain, and one machine can do that:
#
#   .\run_wire.ps1 -AllToolsets      # every MSVC under $env:ND_VS_DIR (or the default)
#
param(
    [string[]] $Toolsets = @(),
    [switch]   $AllToolsets
)
$ErrorActionPreference = "Continue"
$src = $PSScriptRoot            # 脚本自己所在目录：clone 到哪都能跑
$vs    = if ($env:ND_VS_DIR) { $env:ND_VS_DIR } else { "F:\Microsoft Visual Studio\18\Community" }
$vsdev = "$vs\Common7\Tools\VsDevCmd.bat"
$rc = 0

if ($AllToolsets -and $Toolsets.Count -eq 0) {
    $root = Join-Path $vs 'VC\Tools\MSVC'
    if (Test-Path $root) {
        $Toolsets = Get-ChildItem $root -Directory | Sort-Object Name | Select-Object -ExpandProperty Name
    }
    if ($Toolsets.Count -eq 0) { $Toolsets = @('') }     # '' = whatever the VS default is
}
if ($Toolsets.Count -eq 0) { $Toolsets = @('') }         # '' = default toolset, unchanged behaviour

foreach ($ts in $Toolsets) {
    if ($ts) { Write-Host "===== MSVC toolset $ts" }
    else     { Write-Host "===== MSVC default toolset" }
    $verArg = if ($ts) { " -vcvars_ver=$ts" } else { "" }

    foreach ($lang in @("c", "cpp")) {
        $exe = "$src\wire_selftest_$lang.exe"
        $obj = "$src\wire_selftest_$lang.obj"
        Remove-Item $exe, $obj -ErrorAction SilentlyContinue
        if ($lang -eq "c") {
            $flags = "/TC /W4 /WX"
        } else {
            $flags = "/TP /W4 /WX /std:c++17 /EHsc"
        }
        $cmd = "call `"$vsdev`" -arch=x64 -no_logo$verArg >nul 2>&1 && cd /d `"$src`" && " +
               "cl /nologo $flags /I`"$src`" wire_selftest.c /Fe:wire_selftest_$lang.exe /Fo:wire_selftest_$lang.obj"
        Write-Host "  ----- as $lang"
        $buildOut = & cmd.exe /c $cmd 2>&1
        $buildRc = $LASTEXITCODE
        # Print the version line too: when a build fails on one machine and not another,
        # "which compiler" is the first question and it should already be in the log.
        $buildOut | Where-Object { $_ -match "error|warning|Version" } | Select-Object -First 12 | ForEach-Object { "    $_" }
        # Judge the build by its exit code, never by "does an exe exist": a stale exe
        # from the previous language makes that check pass on a failed compile.
        if ($buildRc -ne 0 -or -not (Test-Path $exe)) {
            Write-Host "  BUILD FAILED (rc=$buildRc) as $lang"
            $rc = 1
            continue
        }
        $out = & $exe 2>&1
        $outRc = $LASTEXITCODE
        $out | ForEach-Object { "    $_" }
        if ($outRc -ne 0) { Write-Host "  SELF-TEST FAILED as $lang (rc=$outRc)"; $rc = 1 }
    }
}

if ($rc -eq 0) { Write-Host "wire self-test: PASS as both C and C++" }
exit $rc
