# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
param(
    [int]$timeoutSec = 60,
    [string]$serverIp = "192.168.100.2",
    [string]$clientLocalIp = "192.168.100.3",
    [int]$port = 54340
)
$ErrorActionPreference = "Continue"
$src = $PSScriptRoot            # 脚本自己所在目录：clone 到哪都能跑
$exe = "$src\f1_bringup.exe"

# ---- build -----------------------------------------------------------------
$vs    = if ($env:ND_VS_DIR) { $env:ND_VS_DIR } else { "F:\Microsoft Visual Studio\18\Community" }
$vsdev = "$vs\Common7\Tools\VsDevCmd.bat"
$ndInc = if ($env:ND_NDUTIL_INC) { $env:ND_NDUTIL_INC } else { "D:\rdma\NetworkDirect\src\ndutil" }
$ndLib = if ($env:ND_NDUTIL_LIB) { $env:ND_NDUTIL_LIB } else { "D:\rdma\NetworkDirect\src\x64\Release" }
$mxlInc = if ($env:ND_MLNX_INC) { $env:ND_MLNX_INC } else { "C:\Program Files\Mellanox\MLNX_VPI\IB\SDK\inc\ndv2" }
$inc = "/I`"$src`" /I`"$ndInc`""
if (Test-Path $mxlInc) { $inc += " /I`"$mxlInc`"" }
$cmd = "call `"$vsdev`" -arch=x64 -no_logo >nul 2>&1 && cd /d `"$src`" && " +
       "cl /nologo /W4 /std:c++17 /EHsc $inc f1_bringup.cpp " +
       "/Fe:f1_bringup.exe /Fo:f1_bringup.obj " +
       "/link /LIBPATH:`"$ndLib`" ndutil.lib ws2_32.lib"
Write-Host "building f1_bringup.exe ..."
Remove-Item $exe -ErrorAction SilentlyContinue
$buildOut = & cmd.exe /c $cmd 2>&1
$buildRc = $LASTEXITCODE
$buildOut | Where-Object { $_ -match "error|warning" } | ForEach-Object { "  $_" }
# Judge the build by its exit code and by the artefact being newer than its
# sources, never by "does an exe exist": a stale exe from a previous build makes
# that check pass while the source that just failed to compile is what you think
# you are testing.  The headers are included in the freshness check on purpose -
# a change to nvmeof_wire.h or nvmeof_rdma.h is exactly the kind of edit a
# source-only check misses.
if ($buildRc -ne 0 -or -not (Test-Path $exe)) {
    Write-Host "BUILD FAILED (rc=$buildRc) - refusing to run a stale binary"
    exit 1
}
foreach ($f in @("f1_bringup.cpp", "nvmeof_wire.h", "nvmeof_rdma.h")) {
    $p = Join-Path $src $f
    if ((Get-Item $p).LastWriteTime -gt (Get-Item $exe).LastWriteTime) {
        Write-Host "BUILD STALE: $f is newer than the exe - refusing to run"
        exit 1
    }
}

# ---- run -------------------------------------------------------------------
# Serialise the test suites.  Running one suite while another's processes are
# still winding down produces a spurious failure that looks like a real
# regression: the previous suite's target still holds the RDMA stack, and the
# next suite's queue pair Receive completes as ND_CANCELED with 0 bytes.
# Observed exactly once, and chased as a regression before being understood.
foreach ($n in @("stag_smoketest", "f1_bringup", "f3_io")) {
    Get-Process $n -ErrorAction SilentlyContinue | Stop-Process -Force
}
for ($i = 0; $i -lt 40; $i++) {
    $left = Get-Process stag_smoketest, f1_bringup, f3_io -ErrorAction SilentlyContinue
    if (-not $left) { break }
    Start-Sleep -Milliseconds 250
}
Start-Sleep -Milliseconds 500
$srvOut = "$src\f1_srv.txt"; $cliOut = "$src\f1_cli.txt"
Remove-Item $srvOut, $cliOut -ErrorAction SilentlyContinue

$srv = Start-Process -FilePath $exe -ArgumentList @("-target", $serverIp, "$port", "-serve", "1", "-runfor", "45") `
        -PassThru -RedirectStandardOutput $srvOut
# Wait for READINESS, not a fixed two seconds.  The measured failure mode of a fixed sleep is
# an INITIATOR-side CONNECTION_REFUSED whose target log stops at the banner - i.e. the listener
# never armed - which reads like a protocol bug and is a race.  The target flushes this line
# (f5_interop.cpp, "listener armed"), so polling the file is reliable.  -runfor is the other
# half: it bounds the target's life so it can ALWAYS end by itself, even if it never served a
# controller, because killing it is what leaves the next suite unable to connect (DESIGN 8.78).
$ready = $false
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 250
    if ((Test-Path $srvOut) -and (Select-String -Path $srvOut -Pattern "listener armed" -Quiet)) { $ready = $true; break }
}
if (-not $ready) {
    Write-Host "  [TARGET NEVER ARMED within 15 s - the listener is in the target log]"
    Get-Content $srvOut -ErrorAction SilentlyContinue | ForEach-Object { "    $_" }
}
$cli = Start-Process -FilePath $exe -ArgumentList @("-initiator", $serverIp, "$port", $clientLocalIp) `
        -PassThru -RedirectStandardOutput $cliOut
if (-not $cli.WaitForExit($timeoutSec * 1000)) { $cli.Kill(); Write-Host "  [INITIATOR HUNG - killed]" }
# Let the target end on its own.  If it has to be killed, say so AND settle: the measurement in
# DESIGN 8.78 is that the stack refuses new connections for tens of seconds afterwards, so
# starting the next suite immediately measures the previous kill, not the next suite.
if (-not $srv.WaitForExit(60000)) {
    $srv.Kill()
    Write-Host "  [TARGET KILLED - waiting 20 s for the stack to settle before the next suite]"
    Start-Sleep -Seconds 20
}
Start-Sleep -Milliseconds 300

Write-Host "===== f1_bringup  $clientLocalIp -> $serverIp`:$port"
Write-Host "----- INITIATOR (exit $($cli.ExitCode)):"
if (Test-Path $cliOut) { Get-Content $cliOut | ForEach-Object { "  $_" } }
Write-Host "----- TARGET (exit $($srv.ExitCode)):"
if (Test-Path $srvOut) { Get-Content $srvOut | ForEach-Object { "  $_" } }
