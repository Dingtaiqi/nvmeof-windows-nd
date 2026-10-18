# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# enable_bigio.ps1 - choose how many bytes one NVMe READ/WRITE moves.
#
#   4   = the old in-capsule behaviour (default, always safe)
#   64  = one 64 KiB command, whose payload moves as H2CData/C2HData PDUs
#
# THE DEVICE IS NOT RESTARTED HERE, ON PURPOSE.  On 2026-10-18 12:03:49,
# `pnputil /restart-device ROOT\HDC\0002` on this machine bugchecked with
# 0xCE (P2 = 0x10, DRIVER_UNLOADED_WITHOUT_CANCELLING_PENDING_OPERATIONS)
# because the miniport still had its NVMe/TCP connection and a pending WSK
# request.  That is the same "never unload a driver with an active connection"
# rule the project learned the hard way; a device restart is an unload.
#
# The value is read in HwInitialize (engine attach), so it takes effect on the
# NEXT BOOT.  Set it, reboot, and then run z_bigio_test.exe.  To go back to the
# safe value, run this with -Rollback and reboot again.
param(
    [int]$ChunkKb = 64,
    [switch]$Rollback
)

$ErrorActionPreference = 'Continue'
if ($Rollback) { $ChunkKb = 4 }
if ($ChunkKb -lt 4)   { $ChunkKb = 4 }
if ($ChunkKb -gt 128) { $ChunkKb = 128 }

& reg.exe add 'HKLM\Software\NVMeoFProbe' /v nvmeofnd_chunkkb /t REG_DWORD /d $ChunkKb /f 2>&1 | Out-Null
$now = (Get-ItemProperty 'HKLM:\Software\NVMeoFProbe' -Name nvmeofnd_chunkkb -EA SilentlyContinue).nvmeofnd_chunkkb
Write-Output ('nvmeofnd_chunkkb = ' + $now + ' KiB (applies at the next boot)')
if ($ChunkKb -ne 4) {
    Write-Output ''
    Write-Output 'REBOOT REQUIRED.  Do not restart the device: that is the 0xCE above.'
    Write-Output 'If Z: does not come back after that boot: boot Safe Mode (the miniport does'
    Write-Output 'not load there), run this script with -Rollback, and reboot normally.'
}