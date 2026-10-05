# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
param(
    [int]$timeoutSec = 90,
    [string]$serverIp = "192.168.100.2",
    [string]$clientLocalIp = "192.168.100.3",
    [int]$port = 54380
)
$ErrorActionPreference = "Continue"
$src = $PSScriptRoot            # 脚本自己所在目录：clone 到哪都能跑
$exe = "$src\f6_lifecycle.exe"

$vs    = if ($env:ND_VS_DIR) { $env:ND_VS_DIR } else { "F:\Microsoft Visual Studio\18\Community" }
$vsdev = "$vs\Common7\Tools\VsDevCmd.bat"
$ndInc = if ($env:ND_NDUTIL_INC) { $env:ND_NDUTIL_INC } else { "D:\rdma\NetworkDirect\src\ndutil" }
$ndLib = if ($env:ND_NDUTIL_LIB) { $env:ND_NDUTIL_LIB } else { "D:\rdma\NetworkDirect\src\x64\Release" }
$mxlInc = if ($env:ND_MLNX_INC) { $env:ND_MLNX_INC } else { "C:\Program Files\Mellanox\MLNX_VPI\IB\SDK\inc\ndv2" }
$inc = "/I`"$src`" /I`"$ndInc`""
if (Test-Path $mxlInc) { $inc += " /I`"$mxlInc`"" }
$cmd = "call `"$vsdev`" -arch=x64 -no_logo >nul 2>&1 && cd /d `"$src`" && " +
       "cl /nologo /W4 /std:c++17 /EHsc $inc f6_lifecycle.cpp " +
       "/Fe:f6_lifecycle.exe /Fo:f6_lifecycle.obj " +
       "/link /LIBPATH:`"$ndLib`" ndutil.lib ws2_32.lib"
Write-Host "building f6_lifecycle.exe ..."
Remove-Item $exe -ErrorAction SilentlyContinue
$buildOut = & cmd.exe /c $cmd 2>&1
$buildRc = $LASTEXITCODE
$buildOut | Where-Object { $_ -match "error|warning" } | ForEach-Object { "  $_" }
# Judge the build by its exit code and by the artefact being newer than the
# source, never by "does an exe exist" - a stale exe from a previous build makes
# that check pass while the source that just failed to compile is what you think
# you are testing.
if ($buildRc -ne 0 -or -not (Test-Path $exe)) {
    Write-Host "BUILD FAILED (rc=$buildRc) - refusing to run a stale binary"
    exit 1
}
$srcFile = Join-Path $src "f6_lifecycle.cpp"
if ((Get-Item $srcFile).LastWriteTime -gt (Get-Item $exe).LastWriteTime) {
    Write-Host "BUILD STALE: source is newer than the exe - refusing to run"
    exit 1
}

# Serialise the suites: a previous suite's process still winding down holds the
# RDMA stack, and the next suite's queue pair Receive completes as ND_CANCELED
# with 0 bytes - which reads exactly like a protocol failure.
foreach ($n in @("stag_smoketest", "f1_bringup", "f3_io", "f4_pipeline", "f6_lifecycle")) {
    Get-Process $n -ErrorAction SilentlyContinue | Stop-Process -Force
}
for ($i = 0; $i -lt 60; $i++) {
    $left = Get-Process stag_smoketest, f1_bringup, f3_io, f4_pipeline, f6_lifecycle -ErrorAction SilentlyContinue
    if (-not $left) { break }
    Start-Sleep -Milliseconds 250
}
Start-Sleep -Milliseconds 1500

$srvOut = "$src\f6_srv.txt"; $cliOut = "$src\f6_cli.txt"
Remove-Item $srvOut, $cliOut -ErrorAction SilentlyContinue

$srv = Start-Process -FilePath $exe -ArgumentList @("-target", $serverIp, "$port") `
        -PassThru -RedirectStandardOutput $srvOut
Start-Sleep -Seconds 2
$cli = Start-Process -FilePath $exe -ArgumentList @("-initiator", $serverIp, "$port", $clientLocalIp) `
        -PassThru -RedirectStandardOutput $cliOut
if (-not $cli.WaitForExit($timeoutSec * 1000)) { $cli.Kill(); Write-Host "  [INITIATOR HUNG - killed]" }
if (-not $srv.WaitForExit(20000))              { $srv.Kill(); Write-Host "  [TARGET HUNG - killed]" }
Start-Sleep -Milliseconds 300

Write-Host "===== f6_lifecycle  $clientLocalIp -> $serverIp`:$port"
Write-Host "----- INITIATOR (exit $($cli.ExitCode)):"
if (Test-Path $cliOut) { Get-Content $cliOut | ForEach-Object { "  $_" } }
Write-Host "----- TARGET (exit $($srv.ExitCode)):"
if (Test-Path $srvOut) { Get-Content $srvOut | ForEach-Object { "  $_" } }
