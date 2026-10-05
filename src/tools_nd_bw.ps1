# SPDX-License-Identifier: Apache-2.0
<#
  Pure-link bandwidth: the OFFICIAL NetworkDirect tools, no protocol layer at all.

  Why this exists next to f4_pipeline: f4 measures this project's NVMe-oF path
  (capsules, SGLs, a target that serves commands).  That number is only meaningful
  as a fraction of what the fabric and the ND provider can do when nothing is
  layered on them - and that ceiling belongs to a different tool, the one Mellanox
  ships with WinOF.  Mixing the two up is how "we are 25% slower than the official
  tool" became a three-week detour in the earlier project (see RDMA_TOOLBOX.md
  section 2.3): nd_write_bw's best numbers are CACHE-RESIDENT, and at the largest
  size it supports (8 MiB) it drops to 2.42 GB/s.

  So both ends measure the same thing and both are reported here: the official
  tools across message sizes, and (separately, by the caller) our own stack.

  Usage:
    .\tools_nd_bw.ps1                                  # write/read/send sweep
    .\tools_nd_bw.ps1 -Ops write -Sizes 1MB,8MB -Seconds 10
    .\tools_nd_bw.ps1 -Ops lat -Sizes 512

  Server and client are the SAME machine's two RoCE ports (100.2 <-> 100.3), which
  is how every other test in this repo runs.  Traffic does cross the cable; the
  host's PCIe and memory system serve both directions, so this is a link+host
  number, not a two-machine one.
#>
[CmdletBinding()]
param(
    [string[]] $Ops        = @('write', 'read', 'send'),
    [string[]] $Sizes      = @('64KB', '256KB', '1MB', '4MB', '8MB'),
    [int]      $Seconds    = 10,
    [int]      $Qps        = 1,
    [string]   $ServerIp   = '192.168.100.2',
    [string]   $ClientIp   = '192.168.100.3',
    [string]   $ToolDir    = 'C:\Program Files\Mellanox\MLNX_VPI\IB\Tools',
    [string]   $LogDir     = 'D:\nvmeof\logs'
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

function ConvertTo-Bytes([string]$s) {
    if ($s -match '^(\d+)([KMG]?)B?$') {
        $n = [int64]$Matches[1]
        switch ($Matches[2]) {
            'K' { return $n * 1KB }
            'M' { return $n * 1MB }
            'G' { return $n * 1GB }
            default { return $n }
        }
    }
    throw "cannot parse size '$s'"
}

# The tools print a table; the line we want is the aggregate one and, on the
# client, the reported bandwidth.  Rather than guess the exact column layout
# across versions, both sides' full output is kept and the interesting lines are
# matched.
# The tools print a table whose data row is
#   " #qp #bytes #iterations    MR [Mmps]     Gb/s     CPU Util."
#   " 0   1048576   17221        0.003        24.08    100.00"
# and the latency tools print a "usec" column instead.  Parse the DATA ROW rather
# than grep for "Gb/s": an earlier version of this script matched the header line
# and reported no numbers at all.
function Get-BwRow([string[]]$text) {
    $row = $text | Where-Object { $_ -match '^\s*\d+\s+\d+\s+\d+\s+[\d.]+\s+[\d.]+\s+[\d.]+' } | Select-Object -Last 1
    if (-not $row) { return $null }
    $f = ($row.Trim() -split '\s+')
    return [pscustomobject]@{
        qp    = $f[0]
        bytes = $f[1]
        iters = $f[2]
        mrate = $f[3]
        value = [double]$f[4]        # Gb/s for the -bw tools, usec for the -lat tools
        cpu   = $f[5]
    }
}

$results = @()
foreach ($op in $Ops) {
    $tool = Join-Path $ToolDir ("nd_{0}_bw.exe" -f $op)
    if ($op -eq 'lat') { $tool = Join-Path $ToolDir 'nd_write_lat.exe' }
    if (-not (Test-Path $tool)) { Write-Host "missing tool: $tool"; continue }

    foreach ($s in $Sizes) {
        $bytes = ConvertTo-Bytes $s
        $common = @("-s$bytes", "-D$Seconds", "-q$Qps")
        if ($op -eq 'lat') { $common = @("-s$bytes", "-n1000") }
        $tag = "{0}_{1}_q{2}" -f $op, $s, $Qps
        $srvOut = Join-Path $LogDir "nd_${tag}_srv.txt"
        $cliOut = Join-Path $LogDir "nd_${tag}_cli.txt"

        $srv = Start-Process -FilePath $tool -ArgumentList (@("-S", $ServerIp) + $common) `
                 -PassThru -RedirectStandardOutput $srvOut -WindowStyle Hidden
        Start-Sleep -Seconds 2
        $cli = Start-Process -FilePath $tool -ArgumentList (@("-C", $ServerIp) + $common) `
                 -PassThru -RedirectStandardOutput $cliOut -WindowStyle Hidden
        if (-not $cli.WaitForExit(($Seconds + 60) * 1000)) { $cli.Kill(); Write-Host "  [$tag] client hung" }
        if (-not $srv.WaitForExit(30000))                  { $srv.Kill() }
        Start-Sleep -Milliseconds 400

        $ct = if (Test-Path $cliOut) { Get-Content $cliOut } else { @() }
        $st = if (Test-Path $srvOut) { Get-Content $srvOut } else { @() }
        $r = Get-BwRow $ct
        if (-not $r) { $r = Get-BwRow $st }
        if (-not $r) {
            Write-Host ("{0,-24} no result" -f $tag)
            $results += [pscustomobject]@{ op = $op; size = $s; qp = $Qps; value = $null; cpu = $null }
            continue
        }
        if ($op -eq 'lat') {
            Write-Host ("{0,-24} {1,8:N2} us   cpu {2}" -f $tag, $r.value, $r.cpu)
            $results += [pscustomobject]@{ op = $op; size = $s; qp = $Qps; value = ("{0:N2} us" -f $r.value); cpu = $r.cpu }
        } else {
            $gbs = $r.value / 8.0
            Write-Host ("{0,-24} {1,7:N2} Gb/s = {2,6:N2} GB/s  ({3} of 40G)  cpu {4}" -f `
                        $tag, $r.value, $gbs, ("{0:N0}%" -f (100.0 * $r.value / 40.0)), $r.cpu)
            $results += [pscustomobject]@{ op = $op; size = $s; qp = $Qps; value = ("{0:N2} Gb/s = {1:N2} GB/s" -f $r.value, $gbs); cpu = $r.cpu }
        }
    }
}

Write-Host ""
Write-Host "raw output kept in $LogDir\nd_*.txt (both sides)"
$results | Format-Table -AutoSize | Out-String | Write-Host
