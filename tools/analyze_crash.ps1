# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# analyze_crash.ps1 - what crashed, without a debugger.
#
# Why this exists and why it is not kd.exe: on this machine kd.exe was run three
# times to analyse dumps, and the machine froze all three times.  kd is heavy and
# its failure mode is indistinguishable from a hang, so it is off the table here.
#
# It turns out we never needed it.  Windows itself writes the answer into the
# Windows Error Reporting store when it bugchecks:
#
#   C:\ProgramData\Microsoft\Windows\WER\ReportArchive\Kernel_*\Report.wer
#
# and the field that matters is
#
#   Response.BucketId=AV_nvmeofk2!unknown_function
#
# i.e. <crash type>_<module>!<symbol> - the faulting module, named, in a text file
# of a few kilobytes.  The event log (System, id 1001) supplies the bugcheck code
# and its four parameters.  Between those two, a driver bug is identified in one
# second with no debugger, no symbol download and no risk of freezing anything.
#
# Usage:
#   .\analyze_crash.ps1                 # the newest kernel crash
#   .\analyze_crash.ps1 -All            # every kernel crash WER still has
#   .\analyze_crash.ps1 -BugCheckOnly   # just the event-log view

param(
    [switch]$All,
    [switch]$BugCheckOnly
)

$werDirs = @(
    "$env:ProgramData\Microsoft\Windows\WER\ReportArchive",
    "$env:ProgramData\Microsoft\Windows\WER\ReportQueue"
)

function Get-WerKernelReports {
    $out = @()
    foreach ($d in $werDirs) {
        if (-not (Test-Path $d)) { continue }
        Get-ChildItem $d -Recurse -Filter 'Report.wer' -ErrorAction SilentlyContinue | ForEach-Object {
            $first = (Get-Content $_.FullName -TotalCount 40 -ErrorAction SilentlyContinue) -join "`n"
            if ($first -match 'EventType=BlueScreen' -or $_.Directory.Name -like 'Kernel_*') {
                $out += $_
            }
        }
    }
    return $out | Sort-Object LastWriteTime -Descending
}

function Show-Report($r) {
    $txt = Get-Content $r.FullName -ErrorAction SilentlyContinue
    $bucket = ($txt | Where-Object { $_ -match '^Response\.BucketId=' }) -replace '^Response\.BucketId=', ''
    $type   = ($txt | Where-Object { $_ -match '^EventType=' })        -replace '^EventType=', ''
    $app    = ($txt | Where-Object { $_ -match '^AppName=' })          -replace '^AppName=', ''
    $cab    = ($txt | Where-Object { $_ -match '^Response\.CabId=' })  -replace '^Response\.CabId=', ''

    Write-Output ("  time      : " + $r.LastWriteTime)
    Write-Output ("  event     : " + $type + "   app: " + $app)
    Write-Output ("  BUCKET    : " + $bucket)

    # The bucket is "<something>_<module>!<symbol>" - pull the module out, because
    # that is the single most useful fact in the whole report.
    if ($bucket -match '^([A-Za-z0-9]+)_([^!]+)!(.+)$') {
        Write-Output ("  crash type: " + $matches[1])
        Write-Output ("  MODULE    : " + $matches[2])
        Write-Output ("  symbol    : " + $matches[3])
    } elseif ($bucket -match '^([^!]+)!(.+)$') {
        Write-Output ("  MODULE    : " + $matches[1])
        Write-Output ("  symbol    : " + $matches[2])
    }

    # Parameters, if WER recorded them
    $txt | Where-Object { $_ -match '^(Sig|DynamicSig)\[\d+\]\.(Name|Value)=(BCCode|BCP\d|BucketId|OSVersion)' } |
        Select-Object -First 10 | ForEach-Object { Write-Output ("  " + $_) }
    Write-Output ("  report    : " + $r.FullName)
    Write-Output ("  cab       : " + $cab)
}

Write-Output "=== bugcheck events (System log, id 1001) ==="
Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001} -MaxEvents 8 -ErrorAction SilentlyContinue | ForEach-Object {
    $m = ($_.Message -split "`r?`n") | Where-Object { $_ -match '检测错误|bugcheck|0x' } | Select-Object -First 1
    $dump = ($_.Message -split "`r?`n") | Where-Object { $_ -match '\.dmp' } | Select-Object -First 1
    Write-Output ("  [" + $_.TimeCreated + "] " + $m)
    if ($dump) { Write-Output ("      " + $dump.Trim()) }
}
if ($BugCheckOnly) { return }

Write-Output ""
Write-Output "=== WER kernel reports: which MODULE faulted ==="
$reports = Get-WerKernelReports
if (-not $reports) {
    Write-Output "  (no kernel reports in the WER store)"
    return
}
$take = if ($All) { $reports } else { $reports | Select-Object -First 1 }
foreach ($r in $take) {
    Show-Report $r
    Write-Output ""
}
