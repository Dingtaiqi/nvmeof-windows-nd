# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
param(
    [string]$readLimits = "0,64",
    [int]$timeoutSec = 90,
    [string]$serverIp = "192.168.100.2",
    [string]$clientLocalIp = "192.168.100.3"
)
# ---------------------------------------------------------------------------
#  run_stag.ps1 - the STag / memory-window smoke test, in BOTH read-limit
#  configurations.
#
#  Why two: outboundReadLimit = 0 is not a broken configuration, it is the
#  configuration in which the transport FORBIDS RDMA Read in that direction
#  (0xC000003E, a status with no name in ndstatus.h - see nvmeof_rdma.h).  The
#  suite used to run only readlimit=0 and then assert that Reads succeed, so it
#  reported four failures that were really one configuration fact plus the cascade
#  a refused one-sided op leaves behind.  Now:
#
#    readlimit=0   the assertion is that the Read IS refused; the read path is
#                  reported as SKIP because it cannot be exercised at all;
#    readlimit=64  the assertion is that every Read works: control B, the window
#                  read, the window write, invalidation and the stale-token probe.
#
#  A configuration passes only if its own process printed "failures: 0" AND no
#  [FAIL] line - the same rule the other suites use, because a run that died
#  before printing anything also has no [FAIL].
# ---------------------------------------------------------------------------
$ErrorActionPreference = "Continue"
$src = $PSScriptRoot            # 脚本自己所在目录：clone 到哪都能跑
$exe = "$src\stag_smoketest.exe"

& powershell -NoProfile -ExecutionPolicy Bypass -File "$src\build_stag.ps1" 2>&1 |
    Select-String -Pattern "error|exit=" | ForEach-Object { "  $_" }
if (-not (Test-Path $exe)) { Write-Host "build failed"; exit 1 }

$results = @()
foreach ($rl in ($readLimits -split ',')) {
    $rl = $rl.Trim()
    Get-Process stag_smoketest -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300

    $srvOut = "$src\stag_srv_$rl.txt"; $cliOut = "$src\stag_cli_$rl.txt"
    Remove-Item $srvOut, $cliOut -ErrorAction SilentlyContinue
    $srv = Start-Process -FilePath $exe -ArgumentList @("-server", $serverIp, "54330", "-readlimit", $rl) `
            -PassThru -RedirectStandardOutput $srvOut
    Start-Sleep -Seconds 2
    # -local pins the client to the card's OTHER port.  Without it NdResolveAddress
    # answers with the peer's own address (same subnet, same machine) and the
    # connection becomes provider loopback instead of going over the cable.
    $cli = Start-Process -FilePath $exe -ArgumentList @("-client", $serverIp, "54330", "-readlimit", $rl, "-local", $clientLocalIp) `
            -PassThru -RedirectStandardOutput $cliOut
    if (-not $cli.WaitForExit($timeoutSec * 1000)) { $cli.Kill(); Write-Host "  [CLIENT HUNG - killed]" }
    if (-not $srv.WaitForExit(20000))              { $srv.Kill(); Write-Host "  [SERVER HUNG - killed]" }
    Start-Sleep -Milliseconds 300

    Write-Host "===== readlimit=$rl  client local=$clientLocalIp -> server $serverIp"
    Write-Host "----- CLIENT:"
    $cliLines = if (Test-Path $cliOut) { Get-Content $cliOut } else { @() }
    $cliLines | ForEach-Object { "  $_" }
    Write-Host "----- SERVER:"
    $srvLines = if (Test-Path $srvOut) { Get-Content $srvOut } else { @() }
    $srvLines | ForEach-Object { "  $_" }

    $cliFail = ($cliLines | Select-String -Pattern '^client failures: (\d+)').Matches.Groups[1].Value
    $srvFail = ($srvLines | Select-String -Pattern '^server failures: (\d+)').Matches.Groups[1].Value
    $anyFail = ($cliLines + $srvLines | Select-String -Pattern '\[FAIL\]').Count
    $pass = ($cliFail -eq "0") -and ($srvFail -eq "0") -and ($anyFail -eq 0) -and
            (($cliLines | Select-String -Pattern '\[PASS\]').Count -ge 5)
    $results += [pscustomobject]@{
        readlimit = $rl
        clientFailures = $cliFail
        serverFailures = $srvFail
        failLines = $anyFail
        verdict = if ($pass) { "PASS" } else { "FAIL" }
    }
}

Write-Host ""
$results | Format-Table -AutoSize
if ($results | Where-Object { $_.verdict -ne "PASS" }) {
    Write-Host "RESULT: FAIL"
    exit 1
}
Write-Host "RESULT: PASS - $($results.Count) read-limit configuration(s) behaved as the transport requires"
exit 0
