# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# z_bigio_mt.ps1 - concurrent uncached I/O on Z:, to exercise more than one queue.
#
# A single WriteFile is one SRB, and one SRB walks its chunks on ONE worker, so a
# single-threaded run can never show what multiple connections are worth.  This
# starts N independent PowerShell processes (the DSH file sandbox lets those write
# Z:, unlike an .exe started from the workspace), each doing its own uncached
# write/read file, and reports the aggregate.
#
# Aggregate MB/s = total bytes moved / slowest child's I/O seconds, because the
# children run in parallel.  PowerShell startup is excluded: each child reports
# only its own I/O time.
param(
    [int]$Threads = 4,
    [double]$MiBEach = 0.5,
    [int]$Iterations = 10
)

$ErrorActionPreference = 'Stop'
$outDir = 'D:\rdma'
$procs = @()
$outs = @()

for ($i = 0; $i -lt $Threads; $i++) {
    $out = Join-Path $outDir ("mt{0}.out" -f $i)
    Remove-Item $out -ErrorAction SilentlyContinue
    $arg = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $PSScriptRoot 'z_bigio_ps.ps1') +
           '" -MiB ' + $MiBEach + ' -Path Z:\mt' + $i + '.bin -Iterations ' + $Iterations + ' -Quiet'
    $procs += Start-Process powershell.exe -ArgumentList $arg -PassThru -WindowStyle Hidden -RedirectStandardOutput $out
    $outs += $out
}
$procs | Wait-Process -Timeout 600
Start-Sleep -Milliseconds 300

$totalBytes = [int64]0
$slowest = [double]0
$bad = 0
for ($i = 0; $i -lt $outs.Count; $i++) {
    $lines = Get-Content $outs[$i] -ErrorAction SilentlyContinue
    $tot = $lines | Where-Object { $_ -match '^TOTAL ' } | Select-Object -Last 1
    if (-not $tot) {
        Write-Output ("child {0}: no TOTAL line (exit {1})" -f $i, $procs[$i].ExitCode)
        $bad++
        continue
    }
    $parts = $tot -split ' '
    $b = [int64]$parts[1]
    $s = [double]$parts[2]
    $totalBytes += $b
    if ($s -gt $slowest) { $slowest = $s }
    Write-Output ("child {0}: {1:N0} bytes in {2:N3} s = {3:N2} MB/s" -f $i, $b, $s, (($b / 1MB) / $s))
}

if ($slowest -gt 0) {
    Write-Output ''
    Write-Output ("=== {0} thread(s), {1:N2} MiB each, {2} iterations ===" -f $Threads, $MiBEach, $Iterations)
    Write-Output ("aggregate: {0:N2} MB/s  ({1:N1} MiB moved, slowest child {2:N3} s)" -f (($totalBytes / 1MB) / $slowest), ($totalBytes / 1MB), $slowest)
}
if ($bad -ne 0) { Write-Output ("$bad child(ren) failed") }
