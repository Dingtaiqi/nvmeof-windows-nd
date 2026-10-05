# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
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
# A PORT OF OUR OWN FOR THE LOCAL PAIR.
#
# $port keeps its meaning for the -initiatorOnly case, where the peer is the Linux box
# listening on 4420 and the README documents reaching it by default.  For the local pair
# both ends belong to this script, so the only requirement is a port nothing else uses -
# and 4420 is precisely the one thing it must not be: it is what the ad-hoc measurement
# sessions in this repository use for the bridge's backend, and a process force-killed
# while it owned that listener leaves the RDMA-CM port unusable in a way that neither a
# process list nor netstat shows (DESIGN 8.74(7)).  Measured while 4420 refused every
# connection: f1/f3/f4 ran happily on 543xx over the same two adapters, and 4421 bound
# and served normally throughout.
$localPort = if ($initiatorOnly) { $port } else { 54370 }
if (-not $initiatorOnly) {
    # A DIRTY RDMA-CM PORT MUST NOT LOOK LIKE A TEST FAILURE.
    #
    # The listener's port belongs to the ND provider, not to a process: when a process
    # that owned it is killed with -Force the port can stay stuck in
    # STATUS_SHARING_VIOLATION, and nothing shows it - no process, no netstat entry.
    # The symptom then appears in the NEXT run as an INITIATOR-side ND_CONNECTION_REFUSED,
    # which reads like "the target never started" and sends the reader after the wrong
    # bug (DESIGN 8.74(7) is that afternoon).  This script makes it worse for itself:
    # the loop above force-kills leftovers, and if one of them held the port, the bind
    # three lines later fails.
    #
    # Measured on this machine: 4420 was unusable for every later run while 4421 bound
    # normally throughout, and restarting the adapter cleared it.  So instead of
    # depending on a clean port, walk to the next one - the target prints
    # "listener Bind 0xC0000043" and exits, which is a clean failure, not a hang.
    for ($try = 0; $try -le 3; $try++) {
        Remove-Item $srvOut -ErrorAction SilentlyContinue
        $srvArgs = @("-target", $serverIp, "$localPort")
        if ($TargetAuthKey -ne "") {
            $srvArgs += @("-authkey", $TargetAuthKey, "-authdhgroup", "$TargetDhGroup")
        }
        $srv = Start-Process -FilePath $exe -ArgumentList $srvArgs `
                -PassThru -RedirectStandardOutput $srvOut
        # A FIXED SLEEP IS A RACE, AND IT IS STILL HERE ON PURPOSE.
        #
        # The target now prints "[listener armed] <ip>:<port> is accepting connections"
        # once the listener really is up, which is what this should wait for.  A wait
        # loop was written and REVERTED because it could not be verified: with stdout
        # redirected to a file this program's output is block-buffered, so the armed line
        # can be invisible to the reader for as long as the target runs (measured - the
        # line appeared in f5_srv.txt only after the target was killed, even though the
        # target had printed it).  A wait that cannot see the line reports "the target
        # never armed" while it demonstrably had, which is a worse failure than a sleep.
        # Making this reliable means unbuffering the target's stdout when it is not a
        # console; that is a change to the program, not to this script.
        Start-Sleep -Seconds 2
        $bindFail = $false
        if (Test-Path $srvOut) {
            $txt = Get-Content $srvOut -Raw -ErrorAction SilentlyContinue
            if ($txt -match 'listener Bind') { $bindFail = $true }
        }
        if (-not $bindFail) { break }
        if ($srv -and -not $srv.HasExited) { $null = $srv.WaitForExit(5000) }
        $port++
        Write-Host "  the listener port was held by a previous run; retrying on $port"
    }
}

Write-Host "===== f5_interop  $clientLocalIp -> $serverIp`:$localPort  (subnqn $subnqn)"
$cliArgs = @("-initiator", $serverIp, "$localPort", $clientLocalIp,
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
