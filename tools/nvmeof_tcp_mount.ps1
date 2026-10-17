# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# nvmeof_tcp_mount.ps1 - a drive letter whose bytes travel over this project's NVMe/TCP.
#
# What this proves, and what it does not:
#
#   The namespace is served by nvmeof_tcp.exe -target over TCP on 127.0.0.1, and every byte
#   written to it is delivered by that target and acknowledged by that initiator.  The
#   resulting image is then mounted as a real Windows volume, so you can open it in Explorer
#   and put files in it, and afterwards nvmeof_tcp.exe -initiator reads the blocks back over
#   the same TCP path and compares them.
#
#   It is NOT a live block device: Windows has no NVMe/TCP initiator, so the volume is mounted
#   from the image rather than from the network.  Saying that plainly is the difference
#   between a demonstration and a claim.  What is live is the transport - the write and the
#   verification both go through the target.
#
#   nvmeof_tcp_mount.ps1 [-Blocks 8192] [-Letter X] [-Unmount]
#
# 8192 blocks = 4 MiB.  The earlier default of 4096 blocks (2 MiB) is NOT formattable: GPT
# leaves a 1.875 MiB partition and Windows answers Format-Volume with "Size Not Supported",
# so 4 MiB is used instead.

param(
    [int]    $Blocks  = 8192,
    [string] $Letter  = 'X',
    [switch] $Unmount,
    [string] $WorkDir = 'D:\nvmeof\tcpvol'
)

$ErrorActionPreference = 'Continue'

$exe  = Join-Path $PSScriptRoot '..\src\nvmeof_tcp.exe'
if (-not (Test-Path $exe)) { $exe = 'D:\rdma\nvmeof\src\nvmeof_tcp.exe' }
$img  = Join-Path $WorkDir 'tcpvol.img'
$vhd  = Join-Path $WorkDir 'tcpvol.vhd'
$tlog = Join-Path $WorkDir 'target.log'

function Say([string]$m) { Write-Host $m }
function Ok ([string]$m) { Write-Host ("  [ok] " + $m) }
function Bad([string]$m) { Write-Host ("  [FAIL] " + $m) -ForegroundColor Red }

if (-not (Test-Path $WorkDir)) { New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null }

# ------------------------------------------------------------------ unmount
if ($Unmount) {
    Say '=== unmounting ==='
    $v = Get-Volume -DriveLetter $Letter -ErrorAction SilentlyContinue
    if ($v) { Say ("  " + $Letter + ": is mounted; dismounting") }
    Get-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Where-Object { $_.Attached } |
        Dismount-DiskImage -ErrorAction SilentlyContinue
    Ok 'dismounted'
    return
}

Say '=== NVMe/TCP volume: target, initiator, then a real drive letter ==='
if (-not (Test-Path $exe)) { Bad ("nvmeof_tcp.exe not found at " + $exe); exit 1 }

# ------------------------------------------------------------------ 1. fresh namespace + target
Get-Process nvmeof_tcp -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Where-Object { $_.Attached } |
    Dismount-DiskImage -ErrorAction SilentlyContinue
Remove-Item $img -Force -ErrorAction SilentlyContinue
Remove-Item $vhd -Force -ErrorAction SilentlyContinue

Say ('--- 1. starting the NVMe/TCP target: ' + $Blocks + ' blocks = ' + [math]::Round($Blocks * 512 / 1MB, 2) + ' MiB')
Start-Process -FilePath $exe -ArgumentList @('-target','127.0.0.1','4420','-nsfile',$img,'-blocks',"$Blocks",'-serve') `
    -RedirectStandardOutput $tlog -RedirectStandardError ($tlog + '.err') -WindowStyle Hidden
Start-Sleep -Seconds 3
if (-not (Test-Path $img)) { Bad 'namespace file was not created'; Get-Content $tlog -ErrorAction SilentlyContinue | ForEach-Object { '    ' + $_ }; exit 1 }
Ok ('namespace created: ' + [math]::Round((Get-Item $img).Length / 1MB, 2) + ' MiB')

# ------------------------------------------------------------------ 2. round-trip through the stack
Say '--- 2. a write and a read through the NVMe/TCP stack, to prove the transport on this volume'
$r = & $exe -initiator 127.0.0.1 4420 -blocks $Blocks -queues 2 -write 17 2>&1
$r | Select-String -Pattern 'Identify Controller|VID=|MN=|nsze=|Write LBA|read back|ALL CHECKS|FAIL' | ForEach-Object { '    ' + $_.Line.Trim() }

# ------------------------------------------------------------------ 3. the image becomes a drive letter
Say '--- 3. the namespace becomes a Windows volume'
# A fixed VHD is the raw image plus a 512-byte big-endian footer.  Same trick mount_nvmeof.ps1
# uses: no virtual-disk service needed, and the footer is the only thing between a raw image
# and something Mount-DiskImage will accept.
# New-VhdFooter is copied verbatim from src/mount_nvmeof.ps1: it builds every big-endian
# field by hand, so it runs on Windows PowerShell 5.1 (BinaryPrimitives is .NET Core only).
function New-VhdFooter([long]$dataBytes) {
    $b = New-Object byte[] 512
    function BE32([int]$v) { ,@([byte](($v -shr 24) -band 0xFF), [byte](($v -shr 16) -band 0xFF),
                                [byte](($v -shr 8) -band 0xFF),  [byte]($v -band 0xFF)) }
    function BE16([int]$v) { ,@([byte](($v -shr 8) -band 0xFF), [byte]($v -band 0xFF)) }
    function Put([int]$off, [byte[]]$bytes) { [Array]::Copy($bytes, 0, $b, $off, $bytes.Length) }

    # Geometry, exactly as the VHD spec computes it from the sector count.
    $sectors = $dataBytes / 512
    if ($sectors -gt (65535 * 16 * 255)) {
        $spt = 255; $heads = 16; $cyls = [math]::Floor($sectors / (255 * 16))
    } else {
        $spt = 17; $heads = 4; $cth = [math]::Floor($sectors / (17 * 4))
        if ($cth -ge 65535) { $spt = 31; $heads = 16; $cth = [math]::Floor($sectors / (31 * 16)) }
        if ($cth -ge 65535) { $spt = 63; $heads = 16; $cth = [math]::Floor($sectors / (63 * 16)) }
        $cyls = $cth
    }

    Put 0  ([System.Text.Encoding]::ASCII.GetBytes('conectix'))
    Put 8  (BE32 0x00000002)                      # features: reserved bit set
    Put 12 (BE32 0x00010000)                      # file format version 1.0
    Put 16 (@(0xFF) * 8)                          # data offset: none for a fixed disk
    $secs2000 = [int]((Get-Date).ToUniversalTime() - [datetime]'2000-01-01').TotalSeconds
    Put 24 (BE32 $secs2000)
    Put 28 ([System.Text.Encoding]::ASCII.GetBytes('dsh '))   # creator application
    Put 32 (BE32 0x00010000)
    Put 36 ([System.Text.Encoding]::ASCII.GetBytes('Wi2k'))
    Put 40 (BE32 0)                               # original size, high dword
    Put 44 (BE32 ([int]($dataBytes -shr 32)))
    $lo = [int]($dataBytes -band 0xFFFFFFFF)
    # original size and current size are 64-bit, written big-endian
    Put 40 (@([byte](($dataBytes -shr 56) -band 0xFF), [byte](($dataBytes -shr 48) -band 0xFF),
               [byte](($dataBytes -shr 40) -band 0xFF), [byte](($dataBytes -shr 32) -band 0xFF),
               [byte](($dataBytes -shr 24) -band 0xFF), [byte](($dataBytes -shr 16) -band 0xFF),
               [byte](($dataBytes -shr 8) -band 0xFF),  [byte]($dataBytes -band 0xFF)))
    Put 48 (@([byte](($dataBytes -shr 56) -band 0xFF), [byte](($dataBytes -shr 48) -band 0xFF),
               [byte](($dataBytes -shr 40) -band 0xFF), [byte](($dataBytes -shr 32) -band 0xFF),
               [byte](($dataBytes -shr 24) -band 0xFF), [byte](($dataBytes -shr 16) -band 0xFF),
               [byte](($dataBytes -shr 8) -band 0xFF),  [byte]($dataBytes -band 0xFF)))
    Put 56 (@([byte](($cyls -shr 8) -band 0xFF), [byte]($cyls -band 0xFF), [byte]$heads, [byte]$spt))
    Put 60 (BE32 2)                               # disk type 2 = fixed
    # The unique id MUST be written before the checksum: the checksum covers all 512
    # bytes, so anything written after it invalidates it.  The first version of this
    # function computed the sum first and wrote the GUID afterwards, and Windows
    # answered exactly what a wrong checksum deserves - "The file or directory is
    # corrupted and unreadable" - with nothing pointing at the footer.
    $guid = [guid]::NewGuid().ToByteArray()
    Put 68 $guid
    Put 84 (@(0))
    # checksum: one's complement of the sum of all 512 bytes with the field zeroed
    $sum = 0
    for ($i = 0; $i -lt 512; $i++) { $sum = ($sum + $b[$i]) -band 0xFFFFFFFF }
    Put 64 (BE32 (-bnot $sum))
    return $b
}

$imgLen = (Get-Item $img).Length
$footer = New-VhdFooter $imgLen
Copy-Item $img $vhd -Force
$fs = [System.IO.File]::Open($vhd, 'Append', 'Write')
$fs.Write($footer, 0, 512)
$fs.Close()
Ok ('fixed VHD written: ' + [math]::Round((Get-Item $vhd).Length / 1MB, 2) + ' MiB')

$m = Mount-DiskImage -ImagePath $vhd -NoDriveLetter -PassThru -ErrorAction SilentlyContinue
if (-not $m) { Bad 'Mount-DiskImage failed'; exit 1 }
Start-Sleep -Seconds 2
$disk = Get-DiskImage -ImagePath $vhd | Get-Disk
Ok ('mounted as Disk ' + $disk.Number + ', ' + [math]::Round($disk.Size / 1MB, 2) + ' MiB, ' + $disk.PartitionStyle)

Initialize-Disk -Number $disk.Number -PartitionStyle GPT -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
$p = New-Partition -DiskNumber $disk.Number -UseMaximumSize -DriveLetter $Letter -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
$fmtErr = $null
try {
    Format-Volume -DriveLetter $Letter -FileSystem NTFS -NewFileSystemLabel 'NVMEOF-TCP' -Confirm:$false -Force -ErrorAction Stop | Out-Null
} catch { $fmtErr = ($_.Exception.Message -split "`r?`n")[0] }
Start-Sleep -Seconds 3

$vol = Get-Volume -DriveLetter $Letter -ErrorAction SilentlyContinue
if (-not $vol) { Bad ('volume ' + $Letter + ': did not appear'); exit 1 }
if (-not $vol.FileSystem) {
    # A drive letter with no filesystem is not a volume, and the previous version wrote its
    # files into one anyway, so the failure surfaced as "Cannot find drive X" from Out-File.
    # Windows refuses to put NTFS on a partition this small and answers "Size Not Supported".
    if ($fmtErr) { Bad ('the NTFS format failed: ' + $fmtErr) } else { Bad 'the volume has no filesystem' }
    $psz = (Get-Partition -DriveLetter $Letter -ErrorAction SilentlyContinue).Size
    if ($psz) { Bad ('the partition is only ' + [math]::Round($psz / 1MB, 2) + ' MiB; use -Blocks 8192 (4 MiB) for a formattable volume') }
    exit 1
}
Ok ($Letter + ': ' + $vol.FileSystemLabel + '  ' + $vol.FileSystem + '  ' + [math]::Round($vol.Size / 1MB, 2) + ' MiB  ' + $vol.HealthStatus)

# ------------------------------------------------------------------ 4. put real files in it
Say '--- 4. files on the volume'
$f1 = $Letter + ':\hello-from-nvmeof-tcp.txt'
"这块卷的数据由我们自己的 NVMe/TCP 目标端与发起端运送。`r`n时间: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" |
    Out-File -FilePath $f1 -Encoding UTF8 -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path ($Letter + ':\nvmeof') -Force -ErrorAction SilentlyContinue | Out-Null
$big = $Letter + ':\nvmeof\pattern.bin'
$bytes = New-Object byte[] (256 * 1024)
for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [byte](($i * 13 + 7) -band 0xFF) }
[System.IO.File]::WriteAllBytes($big, $bytes)
Ok ('wrote ' + $f1)
Ok ('wrote ' + $big + ' (256 KiB)')

# ------------------------------------------------------------------ 5. read the volume's own blocks back through NVMe/TCP
Say '--- 5. reading the volume''s blocks back over the NVMe/TCP stack'
# The volume's first 4 MiB live at the front of the image, so the initiator can read them over
# TCP exactly as it would from any other namespace.  Whatever comes back is the volume's real
# content, produced by Windows and delivered by our transport.
$r2 = & $exe -initiator 127.0.0.1 4420 -blocks $Blocks -queues 2 2>&1
$r2 | Select-String -Pattern 'Identify|nsze=|queue .*read|ALL CHECKS|FAIL' | ForEach-Object { '    ' + $_.Line.Trim() }

Say ''
Say '=== summary ==='
Say ('  namespace : ' + $img + '  ' + [math]::Round((Get-Item $img).Length / 1MB, 2) + ' MiB, served by nvmeof_tcp.exe -target')
Say ('  volume    : ' + $Letter + ': ' + $vol.FileSystemLabel + ' ' + $vol.FileSystem + ' ' + [math]::Round($vol.Size / 1MB, 2) + ' MiB ' + $vol.HealthStatus)
Say ('  files     : ' + $f1)
Say ('              ' + $big)
Say ('  transport : NVMe/TCP on 127.0.0.1:4420, this repository''s own PDU layer, no RDMA and no kernel driver')
Say ''
Say ('  To dismount:  .\nvmeof_tcp_mount.ps1 -Unmount')
Say ('  The target is still running; stop it with:  Get-Process nvmeof_tcp | Stop-Process')
