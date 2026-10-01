# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# Build and run the wire-format self-test.
#
# It is built TWICE on purpose - once as C and once as C++ - because the file is
# included from both a .c and a .cpp translation unit in this project, and the
# byte layouts have to hold in both.  MSVC's C mode is where _Static_assert is
# not available, which is the whole reason NVMEOF_STATIC_ASSERT exists; building
# only the C++ side would never exercise that branch.
#
# /W4 /WX on both: a layout warning here is a wire-format bug, not a style nit.
$ErrorActionPreference = "Continue"
$src = $PSScriptRoot            # 脚本自己所在目录：clone 到哪都能跑
$vs    = if ($env:ND_VS_DIR) { $env:ND_VS_DIR } else { "F:\Microsoft Visual Studio\18\Community" }
$vsdev = "$vs\Common7\Tools\VsDevCmd.bat"
$rc = 0

foreach ($lang in @("c", "cpp")) {
    $exe = "$src\wire_selftest_$lang.exe"
    $obj = "$src\wire_selftest_$lang.obj"
    Remove-Item $exe, $obj -ErrorAction SilentlyContinue
    if ($lang -eq "c") {
        $flags = "/TC /W4 /WX"
    } else {
        $flags = "/TP /W4 /WX /std:c++17 /EHsc"
    }
    $cmd = "call `"$vsdev`" -arch=x64 -no_logo >nul 2>&1 && cd /d `"$src`" && " +
           "cl /nologo $flags /I`"$src`" wire_selftest.c /Fe:wire_selftest_$lang.exe /Fo:wire_selftest_$lang.obj"
    Write-Host "===== wire_selftest as $lang"
    $buildOut = & cmd.exe /c $cmd 2>&1
    $buildRc = $LASTEXITCODE
    $buildOut | Where-Object { $_ -match "error|warning" } | ForEach-Object { "  $_" }
    # Judge the build by its exit code, never by "does an exe exist": a stale exe
    # from the previous language makes that check pass on a failed compile.
    if ($buildRc -ne 0 -or -not (Test-Path $exe)) {
        Write-Host "  BUILD FAILED (rc=$buildRc) as $lang"
        $rc = 1
        continue
    }
    $out = & $exe 2>&1
    $outRc = $LASTEXITCODE
    $out | ForEach-Object { "  $_" }
    if ($outRc -ne 0) { Write-Host "  SELF-TEST FAILED as $lang (rc=$outRc)"; $rc = 1 }
}

if ($rc -eq 0) { Write-Host "wire self-test: PASS as both C and C++" }
exit $rc
