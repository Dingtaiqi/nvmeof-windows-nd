# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
<#
    Read the bridge's status file - the one thing an operator can look at while the
    service is running.

    Why this exists: the bridge's LOG cannot be read while it runs.  Windows holds it open
    for the life of the service, so Get-Content answers "used by another process", and the
    README's advice is to stop the service first - which is not a diagnostic procedure but a
    second outage.  f5_interop -statusfile writes a small JSON file instead, atomically, with
    a timestamp that keeps moving while the process is alive.

    Exit codes, so this can be a monitor and not just a pretty print:
      0  fresh status, the process is alive and writing
      2  no status file   - never started with -statusfile, or it was deleted
      3  STALE            - the file stopped moving: the process died or hung
      4  unreadable       - present but not the JSON this script understands

    Example:
      .\tools_bridge_status.ps1
      .\tools_bridge_status.ps1 -StaleSeconds 60 -Json | ConvertFrom-Json
#>
[CmdletBinding()]
param(
    [string] $Path = (Join-Path $env:ProgramData 'nvmeof-windows-nd\bridge-status.json'),
    [int]    $StaleSeconds = 20,
    [switch] $Json
)

$ErrorActionPreference = 'Continue'

if (-not (Test-Path -LiteralPath $Path)) {
    Write-Host "no status file at $Path"
    Write-Host "  the bridge writes one when it is started with -statusfile <path>;"
    Write-Host "  install.ps1 passes it, so a service installed by that script has one."
    exit 2
}

try {
    $st = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
} catch {
    Write-Host "the status file is not readable as JSON: $($_.Exception.Message)"
    Write-Host "  a half-written file should be impossible (the writer renames into place),"
    Write-Host "  so this usually means something else is writing that path."
    exit 4
}

# The timestamp is UTC and the file's clock is the machine's, so age is computed the same
# way the writer measured it.  Get-Date -AsUTC is not in PowerShell 5.1, hence the ToUniversalTime.
$written = [datetime]::Parse($st.writtenUtc, [System.Globalization.CultureInfo]::InvariantCulture,
                             [System.Globalization.DateTimeStyles]::AdjustToUniversal)
$ageSec = [math]::Round(((Get-Date).ToUniversalTime() - $written).TotalSeconds, 1)
$fresh = $ageSec -le $StaleSeconds

if ($Json) {
    # Straight through, plus the two derived fields a monitor wants.
    [pscustomobject]@{
        pid = $st.pid; state = $st.state; attempt = $st.attempt; lastRc = $st.lastRc
        uptimeSec = $st.uptimeSec; writtenUtc = $st.writtenUtc
        statusWrites = $st.statusWrites; ageSec = $ageSec; fresh = $fresh
    } | ConvertTo-Json -Compress
    if (-not $fresh) { exit 3 }
    exit 0
}

Write-Host "bridge status ($Path)"
Write-Host ("  state        : {0}" -f $st.state)
Write-Host ("  pid          : {0}" -f $st.pid)
Write-Host ("  attempt      : {0}   (connect attempts so far)" -f $st.attempt)
Write-Host ("  last result  : rc {0}{1}" -f $st.lastRc, $(if ($st.lastRc -eq 0) { ' (success)' } else { ' (not connected)' }))
Write-Host ("  uptime       : {0} s" -f $st.uptimeSec)
Write-Host ("  status writes: {0}" -f $st.statusWrites)
Write-Host ("  written      : {0}  ({1} s ago)" -f $st.writtenUtc, $ageSec)

if (-not $fresh) {
    Write-Host ""
    Write-Host ("STALE: nothing has written this file for {0} s (limit {1} s)." -f $ageSec, $StaleSeconds)
    Write-Host "  The process is gone or hung.  'Get-Service nvmeofNdBridge' says which of the"
    Write-Host "  two the SCM thinks it is; a process that is Running but stale is hung."
    exit 3
}

if ($st.state -ne 'connecting' -and $st.state -ne 'ended') {
    Write-Host ""
    Write-Host ("note: the process is alive but its state is '{0}' - it is not serving a session." -f $st.state)
}
exit 0
