param(
    [string]$Out   = 'F:\nvmeof\gpt-ns.img',
    [int]   $SizeMB = 64,
    [string]$Label  = 'SURVEYTEST'
)
# ===========================================================================
#  tools_make_test_disk.ps1 - build a GPT + NTFS disk image to test -survey with.
#
#  The point is that the GPT is written by WINDOWS, not by this project: a synthetic
#  image built from my own understanding of the layout would agree with my own
#  parser and prove nothing.  Mount-DiskImage + Initialize-Disk -PartitionStyle GPT +
#  Format-Volume is a reference implementation, so if -survey parses THAT correctly,
#  it will parse the real 1 TB disk.
#
#  Writes:  <Out>          the raw disk image (no VHD footer)
#           <Out>.vhd      the same image with a fixed-VHD footer, for mounting
# ===========================================================================
$ErrorActionPreference = 'Stop'
$vhd = "$Out.vhd"

function New-VhdFooter([long]$dataBytes) {
    $b = New-Object byte[] 512
    function BE32([int]$v) { ,@([byte](($v -shr 24) -band 0xFF), [byte](($v -shr 16) -band 0xFF),
                                [byte](($v -shr 8) -band 0xFF),  [byte]($v -band 0xFF)) }
    function Put([int]$off, [byte[]]$bytes) { [Array]::Copy($bytes, 0, $b, $off, $bytes.Length) }
    $sectors = $dataBytes / 512
    $spt = 17; $heads = 4; $cyls = [math]::Floor($sectors / (17 * 4))
    if ($cyls -ge 65535) { $spt = 31; $heads = 16; $cyls = [math]::Floor($sectors / (31 * 16)) }
    if ($cyls -ge 65535) { $spt = 63; $heads = 16; $cyls = [math]::Floor($sectors / (63 * 16)) }
    Put 0  ([System.Text.Encoding]::ASCII.GetBytes('conectix'))
    Put 8  (BE32 0x00000002); Put 12 (BE32 0x00010000); Put 16 (@(0xFF) * 8)
    Put 24 (BE32 ([int]((Get-Date).ToUniversalTime() - [datetime]'2000-01-01').TotalSeconds))
    Put 28 ([System.Text.Encoding]::ASCII.GetBytes('dsh ')); Put 32 (BE32 0x00010000)
    Put 36 ([System.Text.Encoding]::ASCII.GetBytes('Wi2k'))
    $sz8 = @([byte](($dataBytes -shr 56) -band 0xFF), [byte](($dataBytes -shr 48) -band 0xFF),
             [byte](($dataBytes -shr 40) -band 0xFF), [byte](($dataBytes -shr 32) -band 0xFF),
             [byte](($dataBytes -shr 24) -band 0xFF), [byte](($dataBytes -shr 16) -band 0xFF),
             [byte](($dataBytes -shr 8) -band 0xFF),  [byte]($dataBytes -band 0xFF))
    Put 40 $sz8; Put 48 $sz8
    Put 56 (@([byte](($cyls -shr 8) -band 0xFF), [byte]($cyls -band 0xFF), [byte]$heads, [byte]$spt))
    Put 60 (BE32 2)
    # unique id BEFORE the checksum: the checksum covers all 512 bytes
    Put 68 ([guid]::NewGuid().ToByteArray()); Put 84 (@(0))
    $sum = 0
    for ($i = 0; $i -lt 512; $i++) { $sum = ($sum + $b[$i]) -band 0xFFFFFFFF }
    Put 64 (BE32 (-bnot $sum))
    return $b
}

Get-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Where-Object Attached |
    ForEach-Object { Dismount-DiskImage -ImagePath $vhd | Out-Null; Start-Sleep -Seconds 2 }

$bytes = $SizeMB * 1MB
$fs = [System.IO.File]::Create($vhd)
try {
    $fs.SetLength($bytes)
    # The footer belongs at the END.  SetLength leaves the position at 0, so writing
    # it here without seeking puts it at the front and Windows mounts nothing useful -
    # the same class of mistake as the checksum-before-GUID one in mount_nvmeof.ps1.
    $fs.Seek($bytes, 'Begin') | Out-Null
    $fs.Write((New-VhdFooter $bytes), 0, 512)
} finally { $fs.Dispose() }
Write-Host "built $vhd ($SizeMB MiB, blank)"

Mount-DiskImage -ImagePath $vhd | Out-Null
Start-Sleep -Seconds 3
$disk = Get-DiskImage -ImagePath $vhd | Get-Disk
Write-Host "disk $($disk.Number) bus=$($disk.BusType) size=$([math]::Round($disk.Size/1MB,1)) MiB"

# The reference writer: Windows' own GPT initialisation and NTFS format.
if ($disk.PartitionStyle -eq 'RAW') {
    Initialize-Disk -Number $disk.Number -PartitionStyle GPT | Out-Null
}
$part = New-Partition -DiskNumber $disk.Number -UseMaximumSize
Format-Volume -Partition $part -FileSystem NTFS -NewFileSystemLabel $Label -Confirm:$false | Out-Null
Start-Sleep -Seconds 2
$vol = Get-Partition -DiskNumber $disk.Number | Get-Volume | Where-Object DriveLetter
foreach ($v in $vol) { Write-Host "  volume $($v.DriveLetter): $($v.FileSystemLabel) $($v.FileSystem) $([math]::Round($v.Size/1MB,1)) MiB" }

$dl = ($vol | Select-Object -First 1).DriveLetter
if ($dl) {
    Set-Content -Path "${dl}:\hello-from-windows.txt" -Value "written into a GPT+NTFS test disk" -Encoding UTF8
    Write-Host "  wrote ${dl}:\hello-from-windows.txt"
}

Dismount-DiskImage -ImagePath $vhd | Out-Null
Start-Sleep -Seconds 2

# Strip the footer to get the raw image -survey will look at.
# ($out/$Out are the SAME variable in PowerShell - parameter names are not a
# separate namespace - so the streams get names that cannot collide with the path.)
$srcStream = [System.IO.File]::OpenRead($vhd)
try {
    $dstStream = [System.IO.File]::Create($Out)
    try {
        $buf = New-Object byte[] (1MB); $left = $bytes
        while ($left -gt 0) {
            $got = $srcStream.Read($buf, 0, [math]::Min($buf.Length, $left))
            if ($got -le 0) { throw "unexpected end of $vhd" }
            $dstStream.Write($buf, 0, $got); $left -= $got
        }
    } finally { $dstStream.Dispose() }
} finally { $srcStream.Dispose() }
Write-Host "wrote $Out ($((Get-Item $Out).Length) bytes, raw disk image, no footer)"
