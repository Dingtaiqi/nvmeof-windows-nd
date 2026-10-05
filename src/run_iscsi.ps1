# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# Build and run the iSCSI/SCSI protocol self-test.
#
# It needs no adapter, no initiator and no NetworkDirect SDK: iscsi_selftest.cpp links
# only ws2_32 (nvmeof_iscsi.h includes winsock for the socket type, but the parts this
# test exercises - the PDU builders, the padding rule, the negotiation helpers, the
# sense block, the parked-write table - never touch a socket).  That is what lets CI
# run the iSCSI layer on every push, which it never could before.
#
# /W4 /WX: a wire-layout warning here is a protocol bug, not a style nit.  -AllToolsets
# compiles against every MSVC installed on the machine, because "clean here, error
# there" is a real failure mode this project has already paid for once (DESIGN 8.69).
#
#   .\run_iscsi.ps1
#   .\run_iscsi.ps1 -AllToolsets
param(
    [string[]] $Toolsets = @(),
    [switch]   $AllToolsets
)
$ErrorActionPreference = "Continue"
$src = $PSScriptRoot
$vs    = if ($env:ND_VS_DIR) { $env:ND_VS_DIR } else { "F:\Microsoft Visual Studio\18\Community" }
$vsdev = "$vs\Common7\Tools\VsDevCmd.bat"
$rc = 0

if ($AllToolsets -and $Toolsets.Count -eq 0) {
    $root = Join-Path $vs 'VC\Tools\MSVC'
    if (Test-Path $root) {
        $Toolsets = Get-ChildItem $root -Directory | Sort-Object Name | Select-Object -ExpandProperty Name
    }
}
if ($Toolsets.Count -eq 0) { $Toolsets = @('') }     # '' = the VS default toolset

foreach ($ts in $Toolsets) {
    if ($ts) { Write-Host "===== MSVC toolset $ts" }
    else     { Write-Host "===== MSVC default toolset" }
    $verArg = if ($ts) { " -vcvars_ver=$ts" } else { "" }

    $exe = "$src\iscsi_selftest.exe"
    $obj = "$src\iscsi_selftest.obj"
    Remove-Item $exe, $obj -ErrorAction SilentlyContinue
    $cmd = "call `"$vsdev`" -arch=x64 -no_logo$verArg >nul 2>&1 && cd /d `"$src`" && " +
           "cl /nologo /W4 /WX /std:c++17 /EHsc /I`"$src`" iscsi_selftest.cpp " +
           "/Fe:iscsi_selftest.exe /Fo:iscsi_selftest.obj /link ws2_32.lib"
    $buildOut = & cmd.exe /c $cmd 2>&1
    $buildRc = $LASTEXITCODE
    $buildOut | Where-Object { $_ -match "error|warning|Version" } | Select-Object -First 12 | ForEach-Object { "  $_" }
    # The build is judged by its exit code, never by "does the exe exist": a stale exe
    # from a previous toolset would make that check pass on a failed compile.
    if ($buildRc -ne 0 -or -not (Test-Path $exe)) {
        Write-Host "  BUILD FAILED (rc=$buildRc)"
        $rc = 1
        continue
    }
    $out = & $exe 2>&1
    $outRc = $LASTEXITCODE
    $out | ForEach-Object { "  $_" }
    if ($outRc -ne 0) { Write-Host "  SELF-TEST FAILED (rc=$outRc)"; $rc = 1 }
}

if ($rc -eq 0) { Write-Host "iscsi self-test: PASS" }
exit $rc
