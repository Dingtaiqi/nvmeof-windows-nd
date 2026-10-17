# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# provoke_read.ps1 - perform one small read through the miniport and record
# exactly what it did, without going through diskpart (which is slow and was
# killed twice by short job timeouts).  Reading sectors of the remote disk is
# the same opcode 0x02 CapsuleCmd that diskpart issues, but it can be timed and
# killed cleanly.

param(
    [int]$Disk = 5,
    [long]$Offset = 0,
    [int]$Bytes = 512,
    [string]$Tag = '',
    [string]$Out = 'D:\rdma\nvmeof\evidence\provoke_read_result.txt'
)

$sw = [Diagnostics.Stopwatch]::StartNew()
$line = ''
try
{
    $path = '\\.\PhysicalDrive' + $Disk
    $fs = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    [void]$fs.Seek($Offset, [IO.SeekOrigin]::Begin)
    $buf = New-Object 'byte[]' $Bytes
    $n = $fs.Read($buf, 0, $Bytes)
    $fs.Close()
    $hex = ''
    for ($i = 0; $i -lt 16 -and $i -lt $n; $i++) { $hex += ('{0:X2}' -f $buf[$i]) }
    $line = ('{0} OK {1} disk={2} off={3} read={4} elapsed={5}ms first16={6}' -f (Get-Date -Format 'HH:mm:ss.fff'), $Tag, $Disk, $Offset, $n, $sw.ElapsedMilliseconds, $hex)
}
catch
{
    $line = ('{0} FAILED {1} disk={2} off={3} elapsed={4}ms error={5}' -f (Get-Date -Format 'HH:mm:ss.fff'), $Tag, $Disk, $Offset, $sw.ElapsedMilliseconds, $_.Exception.Message)
}
Add-Content -Path $Out -Value $line -Encoding ASCII
