# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#Requires -RunAsAdministrator
<#
    Remove the NVMe-oF iSCSI bridge service installed by install.ps1.

    Order matters: the iSCSI session has to go before the service, or Windows keeps a
    disk whose backing service is gone (which is how the stale 48 MB disk appeared
    during development - the session outlived the bridge and the disk stayed visible
    until it was logged out).

    -KeepFiles leaves the install and data directories in place (log, bridge.conf).
#>
[CmdletBinding()]
param(
    [string] $ServiceName = 'nvmeofNdBridge',
    [string] $InstallDir  = (Join-Path $env:ProgramFiles 'nvmeof-windows-nd'),
    [string] $DataDir     = (Join-Path $env:ProgramData  'nvmeof-windows-nd'),
    [switch] $KeepFiles
)

$ErrorActionPreference = 'Continue'

# 1. Log out the iSCSI session(s) this bridge is serving.  Only sessions whose target
#    is our own NQN are touched - the user's real NAS portal (however it is named) and
#    any other target must survive an uninstall.
$sessions = & iscsicli SessionList 2>&1 | Out-String
$ids = [regex]::Matches($sessions, 'Session Id\s*:\s*(\S+)') | ForEach-Object { $_.Groups[1].Value }
if ($ids) {
    foreach ($id in $ids) {
        if ($sessions -match [regex]::Escape($id) -and $sessions -match 'nvmeof:bridge0') {
            Write-Host "logging out iSCSI session $id"
            & iscsicli LogoutTarget $id | Out-Null
        }
    }
    Start-Sleep -Seconds 2
}

# 2. Stop and delete the service.
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc) {
    if ($svc.Status -ne 'Stopped') {
        Write-Host "stopping service $ServiceName"
        Stop-Service -Name $ServiceName -Force
        Start-Sleep -Seconds 3
    }
    Write-Host "deleting service $ServiceName"
    & sc.exe delete $ServiceName | Out-Null
} else {
    Write-Host "service $ServiceName is not installed"
}

# 3. Put the initiator's burst parameters back, if this install changed them.
#    install.ps1 saved the originals before touching them; restoring is what keeps an
#    uninstall from leaving a machine quietly reconfigured (DESIGN 8.68).
$tuneScript = Join-Path $PSScriptRoot 'tune_initiator.ps1'
$backup = Join-Path $DataDir 'initiator-tuning.json'
if (Test-Path $backup) {
    if (Test-Path $tuneScript) {
        Write-Host "restoring the initiator parameters saved at $backup"
        & $tuneScript -Restore -SaveFile $backup | ForEach-Object { "  $_" }
    } else {
        Write-Host "initiator parameters were tuned by this install, but $tuneScript is gone - not touching the registry"
    }
} else {
    Write-Host "initiator parameters: nothing was saved by install, leaving the registry alone"
}

# 4. Remove the files.
if (-not $KeepFiles) {
    foreach ($d in @($InstallDir, $DataDir)) {
        if (Test-Path $d) {
            Write-Host "removing $d"
            Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
        }
    }
}

Write-Host ''
Write-Host 'Remaining iSCSI state (the user''s own portals are untouched):'
& iscsicli ListTargetPortals 2>&1 | Select-String '\d+\.\d+\.\d+\.\d+' | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
