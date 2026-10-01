param(
    [string]$TargetIp  = '192.168.100.5',
    [int]   $Port      = 4420,
    [string]$Subnqn    = 'nqn.2024-01.local.rdma:linux-nvmet',
    [string]$LocalIp   = '192.168.100.2',   # port B: during a bridge window port A is a
                                            # bridge member and has no TCP/IP (DESIGN 8.47(13))
    [string]$WorkDir   = 'F:\nvmeof',
    [string]$Letter    = 'X',
    [switch]$Unmount,                 # dismount and (unless -NoPush) write it back
    [switch]$NoFormat,                # leave an existing filesystem alone
    [switch]$NoPush,
    [switch]$KeepImage                # do not refresh the image from the target
)
# ===========================================================================
#  mount_nvmeof.ps1 - an NVMe-oF namespace as a Windows drive letter.
#
#  WHAT THIS IS, EXACTLY.  Windows has no built-in NVMe-oF initiator (that is a
#  Windows Server / Storage Spaces Direct feature, not a client one), so a drive
#  letter cannot come from the operating system's own stack.  What this does
#  instead is put OUR initiator in that role:
#
#      Linux nvmet  --NVMe-oF/RDMA (our NetworkDirect stack)-->  .img on F:
#                                                                  |
#                          fixed VHD (512-byte footer appended) ---+
#                                                                  |
#                        Mount-DiskImage -> NTFS -> X:  <----------+
#
#  So X: is a real Windows volume you can open in Explorer and write files into,
#  and its bytes travel over our own NVMe-oF/RDMA implementation in both
#  directions: -dump fetches the namespace, -Unmount pushes it back and flushes.
#
#  WHAT IT IS NOT: a live block device.  Writes to X: do not reach the target
#  until -Unmount pushes the image back, because nothing in Windows can hand a
#  user-mode process's buffer to the volume stack as a disk.  Saying that plainly
#  is the difference between a demo and a claim.
#
#  USAGE
#      .\mount_nvmeof.ps1                       # fetch from Linux nvmet, mount as X:
#      .\mount_nvmeof.ps1 -TargetIp 192.168.100.2 -Subnqn nqn.2024-01.local.rdma:windows-nd
#                                               # ...or from our own target (no Linux)
#      .\mount_nvmeof.ps1 -Unmount              # dismount and push the bytes back
# ===========================================================================
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$exe = Join-Path $src 'f5_interop.exe'
$img = Join-Path $WorkDir 'nvmeof-volume.img'
$vhd = Join-Path $WorkDir 'nvmeof-volume.vhd'
$log = Join-Path $WorkDir 'mount_nvmeof.log'

function Say([string]$m) { Write-Host $m }
function Ok ([string]$m) { Write-Host "  [ok] $m" }
function Bad([string]$m) { Write-Host "  [FAIL] $m" -ForegroundColor Red }

if (-not (Test-Path $WorkDir)) { New-Item -ItemType Directory -Path $WorkDir | Out-Null }
if (-not (Test-Path $exe)) { Bad "f5_interop.exe not found at $exe"; exit 1 }

# ---------------------------------------------------------------------------
#  VHD footer: a fixed VHD is the raw image plus one 512-byte big-endian footer
# ---------------------------------------------------------------------------
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

function Build-Vhd([string]$imgPath, [string]$vhdPath) {
    $len = (Get-Item $imgPath).Length
    if ($len % 512) { throw "image size $len is not a multiple of 512" }
    $footer = New-VhdFooter $len
    $out = [System.IO.File]::Create($vhdPath)
    try {
        $in = [System.IO.File]::OpenRead($imgPath)
        try { $in.CopyTo($out) } finally { $in.Dispose() }
        $out.Write($footer, 0, 512)
    } finally { $out.Dispose() }
}

function Split-Vhd([string]$vhdPath, [string]$imgPath) {
    # The reverse of Build-Vhd, and it is not optional: while the VHD is mounted,
    # Windows writes into the VHD FILE - the .img is untouched from the moment it was
    # wrapped.  The first version of -Unmount pushed the .img, so it faithfully
    # uploaded the image from BEFORE the files were written: the push reported success,
    # every block moved, and the target got the stale copy.  That is what "the test
    # passed and the data is not there" looks like.
    $len = (Get-Item $vhdPath).Length
    if ($len -lt 512) { throw "$vhdPath is too short to be a VHD" }
    $payload = $len - 512
    $fs = [System.IO.File]::OpenRead($vhdPath)
    try {
        $out = [System.IO.File]::Create($imgPath)
        try {
            $buf = New-Object byte[] (1MB)
            $left = $payload
            while ($left -gt 0) {
                $want = [math]::Min($buf.Length, $left)
                $got = $fs.Read($buf, 0, $want)
                if ($got -le 0) { throw "unexpected end of $vhdPath" }
                $out.Write($buf, 0, $got)
                $left -= $got
            }
        } finally { $out.Dispose() }
    } finally { $fs.Dispose() }
    return $payload
}

function Get-MountedImage([string]$vhdPath) {
    Get-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue |
        Where-Object { $_.Attached }
}

# ---------------------------------------------------------------------------
if ($Unmount) {
    Say "== dismounting $vhd"
    $di = Get-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue
    if (-not $di -or -not $di.Attached) {
        # Not an error, and not a reason to stop: the useful half of -Unmount is
        # putting the volume's bytes back, and that works whether or not this call is
        # the one that dismounted it (the first version returned here and left the
        # updated volume sitting in the VHD, unpushed).
        Say "  (not mounted right now - going straight to the push)"
    } else {
        $disk = $di | Get-Disk
        $vol  = $disk | Get-Partition -ErrorAction SilentlyContinue |
                Get-Volume -ErrorAction SilentlyContinue | Where-Object DriveLetter
        foreach ($v in $vol) { Ok ("volume {0}: {1} {2} free of {3}" -f $v.DriveLetter, $v.FileSystemLabel,
                                   [math]::Round($v.SizeRemaining/1MB,1), [math]::Round($v.Size/1MB,1)) }
        Dismount-DiskImage -ImagePath $vhd | Out-Null
        Start-Sleep -Seconds 2
        Ok "dismounted"
    }

    # The mounted volume's bytes are in the .vhd, NOT in the .img: strip the footer
    # back off so what gets pushed is what Windows actually wrote.
    $bytes = Split-Vhd $vhd $img
    Ok "took the volume back out of the VHD ($([math]::Round($bytes/1MB,1)) MiB in $img)"

    if (-not $NoPush) {
        Say ""
        Say "== pushing the image back to $TargetIp ($Subnqn) over NVMe-oF/RDMA"
        & $exe -initiator $TargetIp $Port $LocalIp -subnqn $Subnqn -push $img 2>&1 |
            Tee-Object -FilePath $log | Select-String -Pattern 'READING|WRITTEN|blocks|written back|MiB/s|failures' |
            ForEach-Object { "  " + $_.Line.Trim() }
        if ($LASTEXITCODE -ne 0) { Bad "the push failed (exit $LASTEXITCODE) - see $log"; exit 1 }
        Ok "the volume's bytes are back in the target's namespace and flushed"
    }
    exit 0
}

# ---------------------------------------------------------------------------
Say "== fetching the namespace from $TargetIp ($Subnqn) over NVMe-oF/RDMA"
if ($KeepImage) {
    Say "  (-KeepImage: reusing the local image, the target is not queried)"
} else {
    & $exe -initiator $TargetIp $Port $LocalIp -subnqn $Subnqn -dump $img 2>&1 |
        Tee-Object -FilePath $log |
        Select-String -Pattern 'READING|blocks|read out|MiB/s|failures' |
        ForEach-Object { "  " + $_.Line.Trim() }
    if ($LASTEXITCODE -ne 0) { Bad "the fetch failed (exit $LASTEXITCODE) - see $log"; exit 1 }
    $mb = [math]::Round((Get-Item $img).Length / 1MB, 1)
    Ok "$img refreshed from the target ($mb MiB)"
}

Say ""
Say "== wrapping it as a fixed VHD and mounting it"
Build-Vhd $img $vhd
Ok "built $vhd ($([math]::Round((Get-Item $vhd).Length/1MB,1)) MiB)"

$old = Get-MountedImage $vhd
if ($old) { Dismount-DiskImage -ImagePath $vhd | Out-Null; Start-Sleep -Seconds 2 }

Mount-DiskImage -ImagePath $vhd | Out-Null
Start-Sleep -Seconds 3
$disk = Get-DiskImage -ImagePath $vhd | Get-Disk

$part = $disk | Get-Partition -ErrorAction SilentlyContinue | Where-Object { $_.Size -gt 0 } | Select-Object -First 1
if (-not $part) {
    if ($NoFormat) { Bad "the image has no partition and -NoFormat was given"; exit 1 }
    Say "  the image carries no partition table - initialising and formatting it"
    Initialize-Disk -Number $disk.Number -PartitionStyle MBR | Out-Null
    $part = New-Partition -DiskNumber $disk.Number -UseMaximumSize
    Format-Volume -Partition $part -FileSystem NTFS -NewFileSystemLabel 'NVMEOF' -Confirm:$false | Out-Null
    Start-Sleep -Seconds 2
    $part = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.Size -gt 0 } | Select-Object -First 1
}

if ($part.DriveLetter -ne $Letter[0]) {
    Set-Partition -DiskNumber $disk.Number -PartitionNumber $part.PartitionNumber -NewDriveLetter $Letter[0]
    Start-Sleep -Seconds 2
}
$v = Get-Volume -DriveLetter $Letter -ErrorAction SilentlyContinue
if (-not $v) { Bad "the volume did not come up as $Letter`:"; exit 1 }

Say ""
Say "=== $Letter`: IS AN NVMe-oF VOLUME ==="
"  label      : $($v.FileSystemLabel)"
"  filesystem : $($v.FileSystem)"
"  size       : $([math]::Round($v.Size/1MB,1)) MiB, free $([math]::Round($v.SizeRemaining/1MB,1)) MiB"
"  backing    : $img  (fetched over NVMe-oF/RDMA from $TargetIp)"
"  mounted as : $vhd"
Say ""
Say "Write files into $Letter`:\ , then run:  .\mount_nvmeof.ps1 -Unmount"
Say "and the bytes go back into the target's namespace (that is what makes it a volume"
Say "and not a copy)."
