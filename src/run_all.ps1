# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# ===========================================================================
#  run_all.ps1 - run the whole suite in order and print one summary table.
#
#  The suites serialise themselves (each one kills and awaits any prior test
#  process), so they must be run in sequence, not in parallel.  This script just
#  does that and reduces each log to a verdict, so "where are we" is one command
#  instead of six.
#
#  A suite is judged by its own rules: the run_*.ps1 scripts already refuse to
#  run a stale binary (delete exe -> check compile exit code -> compare source and
#  header timestamps).  This script adds the assertion-level verdict: any [FAIL]
#  line, or any non-zero "* failures:" count, means the suite did not pass.
# ===========================================================================
param(
    [string[]]$Only = @(),          # e.g. -Only run_f5.ps1,run_f6.ps1
    [int]$TimeoutSec = 1800
)

$ErrorActionPreference = 'Continue'
$src = $PSScriptRoot
$all = @('hygiene.ps1', 'run_xref.ps1', 'run_wire.ps1', 'run_iscsi.ps1', 'run_fuzz.ps1', 'run_auth.ps1', 'run_f1.ps1', 'run_f3.ps1',
         'run_f4.ps1', 'run_f5.ps1', 'run_f5_auth.ps1', 'run_f6.ps1', 'run_f7.ps1',
         'run_stag.ps1')
$scripts = if ($Only.Count -gt 0) { $Only } else { $all }

$rows = @()
$t0 = Get-Date
foreach ($s in $scripts) {
    $path = Join-Path $src $s
    if (-not (Test-Path $path)) {
        $rows += [pscustomobject]@{ Suite = $s; Verdict = 'MISSING'; Detail = ''; Seconds = 0 }
        continue
    }
    Write-Host ("=" * 72)
    Write-Host "running $s ..."
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $path 2>&1
    $rc = $LASTEXITCODE
    $sw.Stop()

    $fails = @($out | Select-String -Pattern '\[FAIL\]').Count
    $counts = @()
    $bad = $false
    foreach ($line in $out) {
        if ($line -match '(\w[\w ]*failures)\s*:\s*(\d+)') {
            $n = [int]$Matches[2]
            $counts += "$($Matches[1])=$n"
            if ($n -ne 0) { $bad = $true }
        }
    }
    $notPassed = $out | Select-String -Pattern 'RESULT:\s*FAIL|BUILD FAILED|BUILD STALE|HUNG|did not exit on its own'
    $verdict = if ($fails -gt 0 -or $bad -or $notPassed -or $rc -ne 0) { 'FAIL' } else { 'PASS' }
    $detail = if ($counts.Count -gt 0) { ($counts -join ', ') }
              elseif ($out | Select-String -Pattern 'RESULT: (PASS|FAIL)') {
                  (($out | Select-String -Pattern 'RESULT:.*').Line | Select-Object -Last 1).Trim() }
              else { '' }

    # Show the lines that matter, not the whole log.
    $out | Select-String -Pattern '\[FAIL\]|failures:|RESULT:|^wire self-test|PASS as both' |
        ForEach-Object { "   " + $_.Line.Trim() }

    $rows += [pscustomobject]@{
        Suite   = $s
        Verdict = $verdict
        Detail  = $detail
        Seconds = [int]$sw.Elapsed.TotalSeconds
    }
}

Write-Host ("=" * 72)
Write-Host ""
$rows | Format-Table Suite, Verdict, Seconds, Detail -AutoSize
$badRows = @($rows | Where-Object Verdict -ne 'PASS')
Write-Host ("total {0:N0} s; {1} of {2} suites passed" -f ((Get-Date) - $t0).TotalSeconds,
            ($rows.Count - $badRows.Count), $rows.Count)
if ($badRows.Count -gt 0) {
    Write-Host "NOT PASSING:" -ForegroundColor Red
    $badRows | ForEach-Object { Write-Host ("  {0}  {1}" -f $_.Suite, $_.Detail) -ForegroundColor Red }
    exit 1
}
Write-Host "all suites passed" -ForegroundColor Green
