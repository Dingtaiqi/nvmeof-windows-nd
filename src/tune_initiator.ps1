# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
<#
  Tune (or restore) the Microsoft iSCSI initiator's burst parameters.

  WHY THIS EXISTS AS A SCRIPT.  Windows' initiator charges a fixed ~16-20 ms per SCSI
  command AND per Data-Out PDU, so throughput is bought by putting more data behind
  each one - and both ends have to be raised together or the negotiation takes the
  smaller value (DESIGN 8.68).  The bridge end is `-iscsimbl` / `-iscsichunk` (install.ps1
  passes both).  This is the other end: three values under the initiator's device-class
  key, which previously lived only as a recipe in the README, so a fresh machine or a
  reinstall silently went back to the slow configuration (11.9 MB/s on a 4 MiB read
  instead of 75-82).

  The values are NOT read at service start: the device instance has to be re-enabled,
  which this script does.  -Restore puts the saved originals back, and -Show prints what
  is set now without touching anything.

  Measured effect (README, DESIGN 8.68): 4 MiB read 11.9 -> 81.9 MB/s, 4 MiB write
  ~900 -> 69 ms, all byte-exact through the three witnesses.

  Examples:
    .\tune_initiator.ps1 -Show
    .\tune_initiator.ps1 -Apply                       # save originals, set, reload device
    .\tune_initiator.ps1 -Restore                     # put the originals back
#>
[CmdletBinding()]
param(
    # 4 MiB: the largest CDB the initiator was measured to issue, and the value the
    # bridge advertises as MaxBurstLength.  Raising it further (16 MiB was tried) did
    # not change the CDB size - the cap is here, not in the bridge.
    [int] $MaxTransferLength = 4194304,
    # 4 MiB as well: the R2T burst size.  Must not be smaller than MaxTransferLength or
    # the CDB is capped below what the initiator would otherwise send.
    [int] $MaxBurstLength = 4194304,
    # 1 MiB per Data-In / Data-Out PDU: fewer PDUs per command, and each one is what
    # the ~16-20 ms is charged for on the write path.
    [int] $MaxRecvDataSegmentLength = 1048576,

    [string] $SaveFile = (Join-Path $env:ProgramData 'nvmeof-windows-nd\initiator-tuning.json'),

    [switch] $Apply,
    [switch] $Restore,
    [switch] $Show,
    # Skip the device reload.  Only useful when the caller knows nothing is logged in;
    # without a reload the registry change has no effect, which is exactly the trap this
    # script exists to close.
    [switch] $NoReload
)

$ErrorActionPreference = 'Stop'

# The initiator's parameters live under the SCSI adapter device class, in whichever
# instance belongs to "Microsoft iSCSI Initiator" - NOT at a fixed index: 0009 on this
# machine, something else on the next one.  Enumerated rather than hardcoded.
function Get-InitiatorKey {
    $class = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4D36E97B-E325-11CE-BFC1-08002BE10318}'
    foreach ($inst in (Get-ChildItem $class -ErrorAction SilentlyContinue)) {
        $p = Get-ItemProperty $inst.PSPath -ErrorAction SilentlyContinue
        if ($p.DriverDesc -eq 'Microsoft iSCSI Initiator') {
            # Report it the way a human types it (HKLM:\...), not the provider-qualified
            # form Get-ChildItem hands back.
            $rel = $inst.PSPath -replace '^.*HKEY_LOCAL_MACHINE', 'HKLM:'
            return Join-Path $rel 'Parameters'
        }
    }
    throw "the Microsoft iSCSI Initiator's device-class key was not found under $class"
}

function Show-Tuning([string]$key) {
    $p = Get-ItemProperty $key
    Write-Host "  $key"
    foreach ($n in 'MaxTransferLength', 'MaxBurstLength', 'MaxRecvDataSegmentLength', 'FirstBurstLength') {
        Write-Host ("    {0,-26} {1}" -f $n, $p.$n)
    }
}

function Restart-InitiatorDevice {
    $dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'ISCSIPRT' }
    if (-not $dev) { Write-Host "  no ROOT\ISCSIPRT device instance: the parameters will apply at next boot"; return }
    foreach ($d in $dev) {
        Write-Host "  re-enabling $($d.InstanceId) so the parameters reload..."
        Disable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
        Enable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 6
}

$key = Get-InitiatorKey
Write-Host "initiator parameters: $key"

if ($Show -or (-not $Apply -and -not $Restore)) {
    Show-Tuning $key
    if (-not $Show) {
        Write-Host ""
        Write-Host "nothing changed.  -Apply to set the tuned values (originals are saved),"
        Write-Host "-Restore to put the saved originals back."
    }
    exit 0
}

if ($Restore) {
    if (-not (Test-Path $SaveFile)) {
        throw "no saved originals at '$SaveFile' - nothing to restore (did -Apply ever run on this machine?)"
    }
    $saved = Get-Content $SaveFile -Raw | ConvertFrom-Json
    foreach ($n in 'MaxTransferLength', 'MaxBurstLength', 'MaxRecvDataSegmentLength') {
        if ($null -ne $saved.$n) {
            Set-ItemProperty $key -Name $n -Value ([int]$saved.$n) -Type DWord
            Write-Host ("  restored {0,-26} {1}" -f $n, $saved.$n)
        }
    }
    if (-not $NoReload) { Restart-InitiatorDevice }
    Write-Host "restored the parameters recorded in $SaveFile"
    Show-Tuning $key
    exit 0
}

# ---- -Apply ------------------------------------------------------------------
$cur = Get-ItemProperty $key
$save = [ordered]@{}
foreach ($n in 'MaxTransferLength', 'MaxBurstLength', 'MaxRecvDataSegmentLength') {
    $save[$n] = [int]$cur.$n
}
$dir = Split-Path -Parent $SaveFile
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
# Only save ORIGINALS: re-running -Apply must not overwrite the backup with the tuned
# values, or -Restore would "restore" the tuning and look like it did nothing.
if (Test-Path $SaveFile) {
    Write-Host "  keeping the existing backup at $SaveFile"
} else {
    $save | ConvertTo-Json | Set-Content -Path $SaveFile -Encoding ASCII
    Write-Host "  saved the originals to $SaveFile"
}

$changed = $false
foreach ($pair in @(@('MaxTransferLength', $MaxTransferLength),
                    @('MaxBurstLength', $MaxBurstLength),
                    @('MaxRecvDataSegmentLength', $MaxRecvDataSegmentLength))) {
    $name = $pair[0]; $want = [int]$pair[1]
    $have = [int]$cur.$name
    if ($have -eq $want) { Write-Host ("  {0,-26} already {1}" -f $name, $want); continue }
    Set-ItemProperty $key -Name $name -Value $want -Type DWord
    Write-Host ("  {0,-26} {1} -> {2}" -f $name, $have, $want)
    $changed = $true
}

if (-not $changed) {
    Write-Host "nothing to change; not touching the device instance (a reload drops sessions)"
} elseif (-not $NoReload) {
    Restart-InitiatorDevice
} else {
    Write-Host "  -NoReload: the new values take effect when the device instance is re-enabled"
}

Show-Tuning $key
