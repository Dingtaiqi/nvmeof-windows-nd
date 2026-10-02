# SPDX-License-Identifier: AGPL-3.0-or-later
<#
  iSCSI bridge throughput, measured through Windows' own initiator.

  Brings up the NVMe-oF backend and the bridge itself, logs in with
  Connect-IscsiTarget, then sweeps block sizes at queue depth 1 and (optionally) with
  N concurrent readers.  It also prints the bridge's own -iscsitime breakdown, because
  "13 ms per command" is not an answer until it says which side spent it - and the
  first version of this measurement blamed the RDMA provider for what turned out to be
  the target's per-command printf (DESIGN 8.68).

  The backend exits after 120 idle seconds, so the stack is started here rather than
  assumed: a benchmark that measures a dead backend is a benchmark of nothing.

  Example:
    .\tools_iscsi_perf.ps1                          # read sweep, QD1
    .\tools_iscsi_perf.ps1 -Concurrent 8            # + 8 concurrent readers
    .\tools_iscsi_perf.ps1 -Chunks 256KB -Total 64MB
#>
[CmdletBinding()]
param(
    [int]      $Drive      = 3,
    [long]     $Total      = 16MB,
    [string[]] $Chunks     = @('8KB', '64KB', '256KB', '1MB'),
    [int]      $Concurrent = 0,
    [string]   $NsFile     = 'D:\nvmeof\ns48.img',
    [string]   $LogDir     = 'D:\nvmeof\logs',
    [string]   $ServerIp   = '192.168.100.2',
    [string]   $LocalIp    = '192.168.100.3',
    [int]      $IscsiPort  = 3260,
    [switch]   $ReadWrite,
    [switch]   $KeepRunning
)

$ErrorActionPreference = 'Continue'
$src = $PSScriptRoot
$exe = Join-Path $src 'f5_interop.exe'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

function ConvertTo-Bytes([string]$s) {
    if ($s -match '^(\d+)([KMGT]?)B?$') {
        $n = [int64]$Matches[1]
        switch ($Matches[2]) { 'K' { return $n * 1KB } 'M' { return $n * 1MB } 'G' { return $n * 1GB } 'T' { return $n * 1TB } default { return $n } }
    }
    throw "cannot parse size '$s'"
}

function Stop-Stack {
    foreach ($p in (Get-CimInstance Win32_Process -Filter "Name='f5_interop.exe'" -ErrorAction SilentlyContinue)) {
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
}

# --- one synchronous reader over a byte range -------------------------------------
function Measure-Read([int]$chunk, [long]$total, [long]$offset) {
    $buf = New-Object byte[] $chunk
    $fs = [System.IO.File]::Open("\\.\PhysicalDrive$Drive", [System.IO.FileMode]::Open,
                                 [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $null = $fs.Seek($offset, [System.IO.SeekOrigin]::Begin)
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $done = 0
        while ($done -lt $total) {
            $n = $fs.Read($buf, 0, [Math]::Min($chunk, $total - $done))
            if ($n -le 0) { break }
            $done += $n
        }
        $sw.Stop()
    } finally { $fs.Dispose() }
    if ($done -le 0) { return $null }
    return [pscustomobject]@{
        MBs = ($done / 1MB) / $sw.Elapsed.TotalSeconds
        ms  = $sw.Elapsed.TotalMilliseconds / ($done / $chunk)
        MiB = $done / 1MB
        sec = $sw.Elapsed.TotalSeconds
    }
}

# --- bring the stack up ------------------------------------------------------------
Stop-Stack
Start-Sleep -Milliseconds 500
Get-IscsiSession -ErrorAction SilentlyContinue | ForEach-Object { iscsicli LogoutTarget $_.SessionIdentifier 2>&1 | Out-Null }
Start-Sleep -Milliseconds 500

$beLog = Join-Path $LogDir 'perf_backend.log'
$brLog = Join-Path $LogDir 'perf_bridge.log'
Remove-Item $beLog, $brLog -ErrorAction SilentlyContinue

Start-Process -FilePath $exe -ArgumentList @('-target', $ServerIp, '4420', '-serve', '0', '-nsfile', $NsFile) `
    -RedirectStandardOutput $beLog -RedirectStandardError "$beLog.err" -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 3
$brArgs = @('-initiator', $ServerIp, '4420', $LocalIp, '-iscsi', "$IscsiPort", '-iscsitime')
if ($ReadWrite) { $brArgs += '-iscsirw' }
Start-Process -FilePath $exe -ArgumentList $brArgs `
    -RedirectStandardOutput $brLog -RedirectStandardError "$brLog.err" -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 7

for ($i = 1; $i -le 6; $i++) {
    if (Get-IscsiSession -ErrorAction SilentlyContinue) { break }
    Connect-IscsiTarget -NodeAddress 'iqn.2024-01.com.nvmeof:bridge0' -IsPersistent $false -ErrorAction SilentlyContinue | Out-Null
    Start-Sleep -Seconds 3
}
Start-Sleep -Seconds 4
if (-not (Get-Disk $Drive -ErrorAction SilentlyContinue)) {
    Write-Host "no iSCSI disk at PhysicalDrive$Drive - the session did not come up"
    Get-Content $brLog -Tail 10
    exit 1
}

Write-Host "===== iSCSI bridge, Windows initiator -> bridge -> NVMe-oF -> $NsFile"
Write-Host ("      disk \\.\PhysicalDrive{0}, {1:N1} MiB read per measurement" -f $Drive, ($Total / 1MB))
$rows = @()
foreach ($c in $Chunks) {
    $bytes = ConvertTo-Bytes $c
    $r = Measure-Read $bytes $Total 0
    if (-not $r) { Write-Host ("  {0,6}  no data" -f $c); continue }
    Write-Host ("  {0,6} blocks, QD1 : {1,8:N1} MB/s   {2,7:N2} ms/command" -f $c, $r.MBs, $r.ms)
    $rows += [pscustomobject]@{ chunk = $c; MBs = [math]::Round($r.MBs, 1); ms = [math]::Round($r.ms, 2) }
}

if ($Concurrent -gt 0) {
    $chunk = ConvertTo-Bytes $Chunks[-1]
    $per = [long]($Total)
    Write-Host ("  {0} concurrent readers, {1} blocks each:" -f $Concurrent, $Chunks[-1])
    $jobs = 1..$Concurrent | ForEach-Object {
        $off = [long](($_ - 1) * $per)
        Start-Job -ScriptBlock {
            param($drive, $chunk, $total, $offset)
            $buf = New-Object byte[] $chunk
            $fs = [System.IO.File]::Open("\\.\PhysicalDrive$drive", [System.IO.FileMode]::Open,
                                         [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
            try {
                $null = $fs.Seek($offset, [System.IO.SeekOrigin]::Begin)
                $sw = [System.Diagnostics.Stopwatch]::StartNew()
                $done = 0
                while ($done -lt $total) {
                    $n = $fs.Read($buf, 0, [Math]::Min($chunk, $total - $done))
                    if ($n -le 0) { break }
                    $done += $n
                }
                $sw.Stop()
            } finally { $fs.Dispose() }
            [pscustomobject]@{ bytes = $done; secs = $sw.Elapsed.TotalSeconds }
        } -ArgumentList $Drive, $chunk, $per, $off
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $res = $jobs | Wait-Job | Receive-Job
    $sw.Stop()
    $jobs | Remove-Job -Force
    $bytes = ($res | Measure-Object -Property bytes -Sum).Sum
    Write-Host ("             aggregate: {0,8:N1} MB/s   ({1:N0} MiB in {2:N2} s)" -f `
                (($bytes / 1MB) / $sw.Elapsed.TotalSeconds), ($bytes / 1MB), $sw.Elapsed.TotalSeconds)
}

# --- what the bridge itself measured ----------------------------------------------
Start-Sleep -Seconds 1
$l = Get-Content $brLog -ErrorAction SilentlyContinue
function Get-Stat([string]$pattern, [string]$field) {
    $l | Select-String $pattern | ForEach-Object {
        if ($_.Line -match "$field (\d+) us") { [int]$Matches[1] }
    }
}
$svc = Get-Stat 'time READ' 'total'
$nv  = Get-Stat 'time READ' 'nvme'
$din = Get-Stat 'time READ' 'datain'
$gap = Get-Stat 'between our last response' ''
function Get-Med([int[]]$v) { if (-not $v -or $v.Count -eq 0) { return $null }; $s = $v | Sort-Object; return $s[[int]($s.Count / 2)] }
Write-Host "      bridge -iscsitime (READ):"
if ($svc) { Write-Host ("        service total : median {0,6} us  mean {1,6:N0} us  (n={2})" -f (Get-Med $svc), ($svc | Measure-Object -Average).Average, $svc.Count) }
if ($nv)  { Write-Host ("          of which nvme: median {0,6} us" -f (Get-Med $nv)) }
if ($din) { Write-Host ("          of which data-in: median {0,4} us" -f (Get-Med $din)) }
if ($gap) { Write-Host ("        gap waiting for the initiator: median {0,5} us" -f (Get-Med $gap)) }

if (-not $KeepRunning) { Stop-Stack }
$rows | Format-Table -AutoSize | Out-String | Write-Host
