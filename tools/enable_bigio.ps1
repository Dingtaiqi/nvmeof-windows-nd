# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# enable_bigio.ps1 - set how many bytes one NVMe READ/WRITE moves, LIVE.
#
#   4   = in-capsule behaviour (the boot default, always safe)
#   64  = one 64 KiB command, payload moved as H2CData/C2HData PDUs
#
# No reboot and no device restart: the miniport re-reads nvmeofnd_chunkkb at most
# once per second, so the next I/O after this script uses the new size.  The driver
# writes 4 back on every attach, so a crash caused by a large value cannot arm the
# next boot with it (that is also why `pnputil /restart-device` must never be used
# here: on 2026-10-18 it bugchecked with 0xCE P2=0x10, an unload with pending work).
param(
    [int]$ChunkKb = 64,
    [switch]$Rollback
)

$ErrorActionPreference = 'Continue'
if ($Rollback) { $ChunkKb = 4 }
if ($ChunkKb -lt 4)   { $ChunkKb = 4 }
if ($ChunkKb -gt 128) { $ChunkKb = 128 }

& reg.exe add 'HKLM\Software\NVMeoFProbe' /v nvmeofnd_chunkkb /t REG_DWORD /d $ChunkKb /f 2>&1 | Out-Null
Start-Sleep -Milliseconds 1500
$now = (Get-ItemProperty 'HKLM:\Software\NVMeoFProbe' -Name nvmeofnd_chunkkb -EA SilentlyContinue).nvmeofnd_chunkkb
Write-Output ('nvmeofnd_chunkkb = ' + $now + ' KiB (the driver applies it on the next I/O)')
Write-Output 'run D:\rdma\nvmeof\src\z_bigio_test.exe to measure and verify Z:'