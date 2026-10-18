# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# enable_bigio.ps1 - switch the NVMe/TCP miniport to one command per 64 KiB and
# verify that Z: survives the device restart.  Rolls back to 4 KiB by itself if it
# does not.
#
# The chunk size is read in HwInitialize (engine attach), so a device restart is
# what applies it - no driver reload and no reboot.  The default in the driver is
# 4 KiB, so everything before this script behaves exactly as it did.
param(
    [int]$ChunkKb = 64,
    [switch]$Rollback
)

$ErrorActionPreference = 'Continue'
if ($Rollback) { $ChunkKb = 4 }

function Set-Chunk([int]$kb) {
    & reg.exe add 'HKLM\Software\NVMeoFProbe' /v nvmeofnd_chunkkb /t REG_DWORD /d $kb /f 2>&1 | Out-Null
    Write-Output ('  nvmeofnd_chunkkb = ' + $kb)
}
function Restart-Miniport() {
    $o = & pnputil /restart-device 'ROOT\HDC\0002' 2>&1 | Out-String
    Write-Output ('  ' + (($o -split "`r?`n" | Where-Object { $_ -match 'success|failed|error' }) -join ' ').Trim())
}
function Wait-Z([int]$seconds) {
    for ($i = 0; $i -lt $seconds; $i++) {
        if (Test-Path 'Z:\') { return $true }
        Start-Sleep -Seconds 1
    }
    return (Test-Path 'Z:\')
}

Write-Output ('=== miniport command size -> ' + $ChunkKb + ' KiB ===')
Set-Chunk $ChunkKb
Restart-Miniport
$ok = Wait-Z 20
Write-Output ('  Z: present = ' + $ok)
if ($ok) {
    $h = (Get-FileHash 'Z:\roundtrip.bin' -Algorithm SHA256).Hash
    Write-Output ('  roundtrip.bin = ' + $h)
    if ($h -ne 'C0DC81BA3F03D8C865E1CF0D400EB298EF7DD3E419788D603CF45704A1455D9B') {
        Write-Output '  [FAIL] the volume content changed; rolling back to 4 KiB'
        $ok = $false
    }
}
if (-not $ok -and $ChunkKb -ne 4) {
    Write-Output '  rolling back to 4 KiB'
    Set-Chunk 4
    Restart-Miniport
    Write-Output ('  Z: present after rollback = ' + (Wait-Z 20))
}
if ($ok) {
    Write-Output ''
    Write-Output 'next: run D:\rdma\nvmeof\src\z_bigio_test.exe (uncached 2 MiB write/read on Z:)'
}
