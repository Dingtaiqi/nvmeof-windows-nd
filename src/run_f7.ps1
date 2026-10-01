# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
param(
    [int]$timeoutSec = 120,
    [string]$serverIp = "192.168.100.2",
    [string]$clientLocalIp = "192.168.100.3",
    [int]$port = 54400
)
$ErrorActionPreference = "Continue"
$src = $PSScriptRoot            # 脚本自己所在目录：clone 到哪都能跑
$exe = "$src\f7_faults.exe"

$vs    = if ($env:ND_VS_DIR) { $env:ND_VS_DIR } else { "F:\Microsoft Visual Studio\18\Community" }
$vsdev = "$vs\Common7\Tools\VsDevCmd.bat"
$ndInc = if ($env:ND_NDUTIL_INC) { $env:ND_NDUTIL_INC } else { "D:\rdma\NetworkDirect\src\ndutil" }
$ndLib = if ($env:ND_NDUTIL_LIB) { $env:ND_NDUTIL_LIB } else { "D:\rdma\NetworkDirect\src\x64\Release" }
$mxlInc = if ($env:ND_MLNX_INC) { $env:ND_MLNX_INC } else { "C:\Program Files\Mellanox\MLNX_VPI\IB\SDK\inc\ndv2" }
$inc = "/I`"$src`" /I`"$ndInc`""
if (Test-Path $mxlInc) { $inc += " /I`"$mxlInc`"" }
$cmd = "call `"$vsdev`" -arch=x64 -no_logo >nul 2>&1 && cd /d `"$src`" && " +
       "cl /nologo /W4 /std:c++17 /EHsc $inc f7_faults.cpp " +
       "/Fe:f7_faults.exe /Fo:f7_faults.obj " +
       "/link /LIBPATH:`"$ndLib`" ndutil.lib ws2_32.lib"
Write-Host "building f7_faults.exe ..."
Remove-Item $exe -ErrorAction SilentlyContinue
$buildOut = & cmd.exe /c $cmd 2>&1
$buildRc = $LASTEXITCODE
$buildOut | Where-Object { $_ -match "error|warning" } | ForEach-Object { "  $_" }
if ($buildRc -ne 0 -or -not (Test-Path $exe)) {
    Write-Host "BUILD FAILED (rc=$buildRc) - refusing to run a stale binary"
    exit 1
}
$srcFile = Join-Path $src "f7_faults.cpp"
if ((Get-Item $srcFile).LastWriteTime -gt (Get-Item $exe).LastWriteTime) {
    Write-Host "BUILD STALE: source is newer than the exe - refusing to run"
    exit 1
}

foreach ($n in @("stag_smoketest", "f1_bringup", "f3_io", "f4_pipeline", "f6_lifecycle", "f7_faults")) {
    Get-Process $n -ErrorAction SilentlyContinue | Stop-Process -Force
}
for ($i = 0; $i -lt 60; $i++) {
    $left = Get-Process stag_smoketest, f1_bringup, f3_io, f4_pipeline, f6_lifecycle, f7_faults -ErrorAction SilentlyContinue
    if (-not $left) { break }
    Start-Sleep -Milliseconds 250
}
Start-Sleep -Milliseconds 1500

# ---------------------------------------------------------------------------
#  Case A: nothing is listening at all.
# ---------------------------------------------------------------------------
Write-Host "===== f7_faults  case A: no target ever existed"
$aCli = "$src\f7_a_cli.txt"
Remove-Item $aCli -ErrorAction SilentlyContinue
$a = Start-Process -FilePath $exe -ArgumentList @("-initiator", $serverIp, "$port", $clientLocalIp, "-phase", "deaf") `
        -PassThru -RedirectStandardOutput $aCli
if (-not $a.WaitForExit($timeoutSec * 1000)) { $a.Kill(); Write-Host "  [HUNG - killed]" }
Start-Sleep -Milliseconds 300
if (Test-Path $aCli) { Get-Content $aCli | ForEach-Object { "  $_" } }

Start-Sleep -Seconds 3

# ---------------------------------------------------------------------------
#  Case B: the target serves four commands and then vanishes mid-stream.
# ---------------------------------------------------------------------------
Write-Host "===== f7_faults  case B: target dies mid-stream"
$bSrv = "$src\f7_b_srv.txt"; $bCli = "$src\f7_b_cli.txt"
Remove-Item $bSrv, $bCli -ErrorAction SilentlyContinue
$srv = Start-Process -FilePath $exe -ArgumentList @("-target", $serverIp, "$port", "-die-after", "4") `
        -PassThru -RedirectStandardOutput $bSrv
Start-Sleep -Seconds 2
$cli = Start-Process -FilePath $exe -ArgumentList @("-initiator", $serverIp, "$port", $clientLocalIp, "-phase", "live") `
        -PassThru -RedirectStandardOutput $bCli
$cliDone = $cli.WaitForExit($timeoutSec * 1000)
if (-not $cliDone) { $cli.Kill(); Write-Host "  [INITIATOR HUNG - killed]" }
if (-not $srv.WaitForExit(10000)) { $srv.Kill() }
Start-Sleep -Milliseconds 300

Write-Host "----- TARGET:"
if (Test-Path $bSrv) { Get-Content $bSrv | ForEach-Object { "  $_" } }
Write-Host "----- INITIATOR:"
if (Test-Path $bCli) { Get-Content $bCli | ForEach-Object { "  $_" } }

if (-not $cliDone) {
    Write-Host "RESULT: the initiator did not exit on its own - this is the hang the test exists for"
    exit 1
}

Start-Sleep -Seconds 1

# ---------------------------------------------------------------------------
#  Case C: the reverse fault - the initiator vanishes, and the target has to
#  notice by itself.  The target has no idle timeout, so a target that does not
#  treat a cancelled Receive as the end of its queue pair sits there forever
#  holding a queue pair for a host that no longer exists.  This case is the one
#  that pins that behaviour: the target must exit on its own, well inside the
#  15 s window, without being killed.
# ---------------------------------------------------------------------------
Write-Host "===== f7_faults  case C: initiator vanishes, target must notice"
$cSrv = "$src\f7_c_srv.txt"; $cCli = "$src\f7_c_cli.txt"
Remove-Item $cSrv, $cCli -ErrorAction SilentlyContinue
$srvC = Start-Process -FilePath $exe -ArgumentList @("-target", $serverIp, "$port") `
        -PassThru -RedirectStandardOutput $cSrv
Start-Sleep -Seconds 2
$cliC = Start-Process -FilePath $exe -ArgumentList @("-initiator", $serverIp, "$port", $clientLocalIp, "-phase", "vanish") `
        -PassThru -RedirectStandardOutput $cCli
if (-not $cliC.WaitForExit($timeoutSec * 1000)) { $cliC.Kill(); Write-Host "  [INITIATOR HUNG - killed]" }
$targetExited = $srvC.WaitForExit(15000)
if (-not $targetExited) { $srvC.Kill() }
Start-Sleep -Milliseconds 300

Write-Host "----- TARGET:"
if (Test-Path $cSrv) { Get-Content $cSrv | ForEach-Object { "  $_" } }
Write-Host "----- INITIATOR:"
if (Test-Path $cCli) { Get-Content $cCli | ForEach-Object { "  $_" } }

$cLog = if (Test-Path $cSrv) { Get-Content $cSrv -Raw } else { "" }
if (-not $targetExited) {
    Write-Host "RESULT: the target kept running after the initiator vanished - leaked queue pair"
    exit 1
}
if ($cLog -notmatch "the initiator has gone") {
    Write-Host "RESULT: the target exited but did not report why it knew the initiator was gone"
    exit 1
}
Write-Host "RESULT: case C passed - the target noticed the initiator was gone and exited on its own"
