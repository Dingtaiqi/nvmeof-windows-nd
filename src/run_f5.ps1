# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
param(
    [int]$timeoutSec = 120,
    [string]$serverIp = "192.168.100.2",
    [string]$clientLocalIp = "192.168.100.3",
    [int]$port = 4420,
    [string]$subnqn = "nqn.2024-01.local.rdma:windows-nd",
    # How many I/O queues the initiator asks for.  Four by default so the suite
    # exercises the multi-queue path on every run: this is the number the host
    # requests, and the target answers with what it really has (DESIGN 8.49).
    [int]$queues = 4,
    # Discovery instead of I/O: connect to the well-known discovery NQN and read the
    # discovery log page.  That is what `nvme discover` does (DESIGN 8.50).
    [switch]$Discover,
    [switch]$initiatorOnly,
    # Build and stop.  The build recipe (include paths, ndutil.lib, the VS dev shell)
    # lives here and nowhere else: a second copy in another script is a second thing
    # to keep in step, and the one that drifts is the one that is not being run.
    [switch]$BuildOnly,
    # DH-HMAC-CHAP: make our TARGET require authentication from the host, using this
    # "DHHC-1:.." key.  Empty means the target accepts a host without auth, which is
    # what every non-auth test in this project expects.
    [string]$TargetAuthKey = "",
    [int]$TargetDhGroup = 2048,
    # The key our INITIATOR presents when the target's Connect result sets ATR.
    # -AuthKeyFile is the one to prefer: an argument is visible in the process table
    # and in the shell's history, and this is a long-lived credential.  The file's
    # first line is the host key, the second (optional) the controller key - the same
    # format the peer-side scripts use.
    [string]$AuthKey = "",
    [string]$AuthKeyFile = "",
    [string]$AuthCtrlKey = ""
)
# F5 - interop harness.
#
# Against Linux, run the target side on the Linux box with linux/nvmet_setup.sh and
# start only the initiator here:
#   .\run_f5.ps1 -initiatorOnly -subnqn nqn.2024-01.local.rdma:linux-nvmet
# and for the reverse direction, start our target here and run `nvme connect` on
# the Linux box against port 4420.
$ErrorActionPreference = "Continue"
$src = $PSScriptRoot            # 脚本自己所在目录：clone 到哪都能跑
$exe = "$src\f5_interop.exe"

$vs    = if ($env:ND_VS_DIR) { $env:ND_VS_DIR } else { "F:\Microsoft Visual Studio\18\Community" }
$vsdev = "$vs\Common7\Tools\VsDevCmd.bat"
$ndInc = if ($env:ND_NDUTIL_INC) { $env:ND_NDUTIL_INC } else { "D:\rdma\NetworkDirect\src\ndutil" }
$ndLib = if ($env:ND_NDUTIL_LIB) { $env:ND_NDUTIL_LIB } else { "D:\rdma\NetworkDirect\src\x64\Release" }
$mxlInc = if ($env:ND_MLNX_INC) { $env:ND_MLNX_INC } else { "C:\Program Files\Mellanox\MLNX_VPI\IB\SDK\inc\ndv2" }
$inc = "/I`"$src`" /I`"$ndInc`""
if (Test-Path $mxlInc) { $inc += " /I`"$mxlInc`"" }
$cmd = "call `"$vsdev`" -arch=x64 -no_logo >nul 2>&1 && cd /d `"$src`" && " +
       "cl /nologo /W4 /std:c++17 /EHsc $inc f5_interop.cpp " +
       "/Fe:f5_interop.exe /Fo:f5_interop.obj " +
       "/link /LIBPATH:`"$ndLib`" ndutil.lib ws2_32.lib advapi32.lib"
Write-Host "building f5_interop.exe ..."
Remove-Item $exe -ErrorAction SilentlyContinue
$buildOut = & cmd.exe /c $cmd 2>&1
$buildRc = $LASTEXITCODE
$buildOut | Where-Object { $_ -match "error|warning" } | ForEach-Object { "  $_" }
if ($buildRc -ne 0 -or -not (Test-Path $exe)) {
    Write-Host "BUILD FAILED (rc=$buildRc) - refusing to run a stale binary"
    exit 1
}
if ($BuildOnly) { Write-Host "build only: OK"; exit 0 }
$srcFile = Join-Path $src "f5_interop.cpp"
$hdr = Join-Path $src "nvmeof_wire.h"
if ((Get-Item $srcFile).LastWriteTime -gt (Get-Item $exe).LastWriteTime -or
    (Get-Item $hdr).LastWriteTime -gt (Get-Item $exe).LastWriteTime) {
    Write-Host "BUILD STALE: source or header is newer than the exe - refusing to run"
    exit 1
}

foreach ($n in @("stag_smoketest", "f1_bringup", "f3_io", "f4_pipeline", "f6_lifecycle",
                 "f7_faults", "f5_interop")) {
    Get-Process $n -ErrorAction SilentlyContinue | Stop-Process -Force
}
for ($i = 0; $i -lt 60; $i++) {
    $left = Get-Process stag_smoketest, f1_bringup, f3_io, f4_pipeline, f6_lifecycle,
            f7_faults, f5_interop -ErrorAction SilentlyContinue
    if (-not $left) { break }
    Start-Sleep -Milliseconds 250
}
Start-Sleep -Milliseconds 500

$cliOut = "$src\f5_cli.txt"
Remove-Item $cliOut -ErrorAction SilentlyContinue

$srv = $null
$srvOut = "$src\f5_srv.txt"
if (-not $initiatorOnly) {
    Remove-Item $srvOut -ErrorAction SilentlyContinue
    $srvArgs = @("-target", $serverIp, "$port")
    if ($TargetAuthKey -ne "") {
        $srvArgs += @("-authkey", $TargetAuthKey, "-authdhgroup", "$TargetDhGroup")
    }
    $srv = Start-Process -FilePath $exe -ArgumentList $srvArgs `
            -PassThru -RedirectStandardOutput $srvOut
    Start-Sleep -Seconds 2
}

Write-Host "===== f5_interop  $clientLocalIp -> $serverIp`:$port  (subnqn $subnqn)"
$cliArgs = @("-initiator", $serverIp, "$port", $clientLocalIp,
             "-subnqn", $subnqn, "-hostnqn",
             "nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f5-0001",
             "-queues", "$queues")
# Read the key HERE, once, and only keep it in memory: it must not be echoed, and it
# must not be re-read from disk by each invocation.
if ($AuthKeyFile -ne "") {
    if (-not (Test-Path $AuthKeyFile)) { Write-Host "missing -AuthKeyFile $AuthKeyFile"; exit 2 }
    $kl = @(Get-Content $AuthKeyFile | Where-Object { $_.Trim() -ne '' })
    $AuthKey = $kl[0].Trim()
    if ($kl.Count -gt 1 -and $AuthCtrlKey -eq "") { $AuthCtrlKey = $kl[1].Trim() }
    if ($AuthKey -notlike 'DHHC-1:*') { Write-Host "$AuthKeyFile line 1 is not a DHHC-1 key"; exit 2 }
    Write-Host ("  auth: host key from {0} ({1}...), controller key: {2}" -f
                $AuthKeyFile, $AuthKey.Substring(0, [Math]::Min(16, $AuthKey.Length)),
                $(if ($AuthCtrlKey) { 'yes' } else { 'none' }))
}
if ($AuthKey -ne "") {
    $cliArgs += @("-authkey", $AuthKey)
    if ($AuthCtrlKey -ne "") { $cliArgs += @("-authctrlkey", $AuthCtrlKey) }
}
if ($Discover) { $cliArgs += "-discover" }
$cli = Start-Process -FilePath $exe `
        -ArgumentList $cliArgs `
        -PassThru -RedirectStandardOutput $cliOut
$cliDone = $cli.WaitForExit($timeoutSec * 1000)
if (-not $cliDone) { $cli.Kill(); Write-Host "  [INITIATOR HUNG - killed]" }

if ($srv) {
    if (-not $srv.WaitForExit(20000)) { $srv.Kill(); Write-Host "  [TARGET HUNG - killed]" }
}
Start-Sleep -Milliseconds 300

if (Test-Path $cliOut) { Get-Content $cliOut | ForEach-Object { "  $_" } }
if (-not $initiatorOnly -and (Test-Path $srvOut)) {
    Write-Host "----- TARGET:"
    Get-Content $srvOut | ForEach-Object { "  $_" }
}

if (-not $cliDone) {
    Write-Host "RESULT: the initiator did not exit on its own"
    exit 1
}
