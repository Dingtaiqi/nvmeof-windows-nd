# SPDX-License-Identifier: AGPL-3.0-or-later
<#
  End-to-end byte check for the iSCSI bridge's WRITE path.

  Writes a deterministic pattern to a disk the Windows iSCSI initiator mounted from
  the bridge, reads it back through that same initiator, sends SYNCHRONIZE CACHE
  through a SCSI pass-through (-> NVMe-oF FLUSH -> the backend's namespace file) and
  then compares the bytes the namespace file actually holds.

  SYNCHRONIZE CACHE IS SENT EXPLICITLY, and that is not a detail: FlushFileBuffers on
  a raw disk handle does not reach the wire here (measured - the bridge log showed no
  CDB 35 after it, and Set-Disk -IsOffline produced none either), so a "the file
  matches" claim without a pass-through flush would be checking an unflushed cache.

  Three witnesses of the same bytes on purpose: the initiator's read-back proves the
  iSCSI data path, the namespace file proves the bytes crossed the RDMA link into the
  backend, and the target's own trace (run the bridge with -iscsitrace) shows which
  PDUs carried them.  A single "the write succeeded" is not evidence about any of the
  three.

  Example:
    .\tools_iscsi_write.ps1 -Drive 3 -Offset 0 -Size 4MB
    .\tools_iscsi_write.ps1 -Drive 3 -Offset 0 -Size 64KB -NoFileCheck

  The pattern is SHA-256 in counter mode, so it is reproducible in any language and
  does not depend on System.Random's algorithm.
#>
[CmdletBinding()]
param(
    [int]    $Drive      = 3,
    [long]   $Offset     = 0,
    [long]   $Size       = 4MB,
    [string] $NsFile     = 'D:\nvmeof\ns48.img',
    [switch] $NoFileCheck,
    [switch] $NoFlush
)

$ErrorActionPreference = 'Stop'

# SCSI_PASS_THROUGH_DIRECT as the storage stack defines it: natural alignment and a
# 16-byte CDB, sizeof == 56 on x64.  A Pack=1 or 32-byte-CDB variant marshals a
# different Length and DeviceIoControl rejects it with error 1306.
$scsiSrc = @'
using System;
using System.Runtime.InteropServices;
public static class NvmeofScsi {
    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2;
    const uint OPEN_EXISTING = 3;
    const uint IOCTL_SCSI_PASS_THROUGH_DIRECT = 0x4D014;

    [StructLayout(LayoutKind.Sequential)]
    public struct SPT_DIRECT {
        public ushort Length; public byte ScsiStatus; public byte PathId; public byte TargetId;
        public byte Lun; public byte CdbLength; public byte SenseInfoLength; public byte DataIn;
        public uint DataTransferLength; public uint TimeOutValue; public IntPtr DataBuffer;
        public uint SenseInfoOffset;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] Cdb;
    }
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa,
                                     uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr inBuf, uint inSize,
                                       IntPtr outBuf, uint outSize, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);

    public static string Send(int drive, byte[] cdb) {
        if (cdb.Length > 16) throw new Exception("CDB longer than 16 bytes");
        string path = @"\\.\PhysicalDrive" + drive;
        IntPtr h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (h == (IntPtr)(-1)) throw new Exception("CreateFile: " + Marshal.GetLastWin32Error());
        try {
            var s = new SPT_DIRECT();
            s.Length = (ushort)Marshal.SizeOf(typeof(SPT_DIRECT));
            s.Cdb = new byte[16];
            s.CdbLength = (byte)cdb.Length;
            Array.Copy(cdb, s.Cdb, cdb.Length);
            s.DataIn = 0;
            s.TimeOutValue = 120;
            int sz = Marshal.SizeOf(typeof(SPT_DIRECT));
            IntPtr buf = Marshal.AllocHGlobal(sz);
            try {
                Marshal.StructureToPtr(s, buf, false);
                uint ret;
                if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, buf, (uint)sz,
                                     buf, (uint)sz, out ret, IntPtr.Zero))
                    throw new Exception("DeviceIoControl: " + Marshal.GetLastWin32Error());
                var r = (SPT_DIRECT)Marshal.PtrToStructure(buf, typeof(SPT_DIRECT));
                return "ScsiStatus=0x" + r.ScsiStatus.ToString("X2");
            } finally { Marshal.FreeHGlobal(buf); }
        } finally { CloseHandle(h); }
    }
}
'@
Add-Type -TypeDefinition $scsiSrc -ErrorAction Stop

function New-DetPattern([int]$n) {
    $out = New-Object byte[] $n
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $nchunks = [int][Math]::Ceiling($n / 32.0)
    for ($k = 0; $k -lt $nchunks; $k++) {
        $h = $sha.ComputeHash([System.Text.Encoding]::ASCII.GetBytes("nvmeof-nd-wtest:$k"))
        $copy = [Math]::Min(32, $n - ($k * 32))
        [Array]::Copy($h, 0, $out, $k * 32, $copy)
    }
    $sha.Dispose()
    # Return the array as ONE object.  A bare `return $out` unrolls it into the
    # pipeline, the caller ends up with an Object[] instead of a byte[], and the timed
    # FileStream.Write() then converts four million elements one at a time: measured
    # 900-1000 ms for 4 MiB, i.e. this tool's "write" number was mostly PowerShell
    # marshalling and said nothing about the bridge.
    return , $out
}

function Get-Sha256Hex([byte[]]$bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $h = $sha.ComputeHash($bytes)
    $sha.Dispose()
    return ([System.BitConverter]::ToString($h) -replace '-', '').ToLower()
}

function Get-FirstDiff([byte[]]$a, [byte[]]$b) {
    $n = [Math]::Min($a.Length, $b.Length)
    for ($i = 0; $i -lt $n; $i++) { if ($a[$i] -ne $b[$i]) { return $i } }
    if ($a.Length -ne $b.Length) { return $n }
    return -1
}

$dev = "\\.\PhysicalDrive$Drive"
Write-Host "pattern    : $Size B at offset $Offset (SHA-256 counter mode)"
$pat = New-DetPattern([int]$Size)
$patHash = Get-Sha256Hex $pat
Write-Host "             sha256=$patHash"

$fs = [System.IO.File]::Open($dev, [System.IO.FileMode]::Open,
                             [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
try {
    $null = $fs.Seek($Offset, [System.IO.SeekOrigin]::Begin)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $fs.Write($pat, 0, $pat.Length)
    $fs.Flush($true)          # FlushFileBuffers - does NOT reach the wire; see below
    $sw.Stop()
    Write-Host "write      : $Size B in $($sw.ElapsedMilliseconds) ms (buffered handle + FlushFileBuffers)"

    $null = $fs.Seek($Offset, [System.IO.SeekOrigin]::Begin)
    $back = New-Object byte[] ([int]$Size)
    $got = 0
    while ($got -lt $back.Length) {
        $n = $fs.Read($back, $got, $back.Length - $got)
        if ($n -le 0) { throw "read-back stopped after $got of $($back.Length) byte(s)" }
        $got += $n
    }
} finally {
    $fs.Dispose()
}

if (-not $NoFlush) {
    # SYNCHRONIZE CACHE(10), LBA 0, number of blocks 0 = "all remaining".  This is the
    # only thing in this script that actually reaches the backend's file: the
    # FlushFileBuffers above is a promise Windows keeps locally.
    $sw2 = [System.Diagnostics.Stopwatch]::StartNew()
    $st = [NvmeofScsi]::Send($Drive, [byte[]]@(0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0))
    $sw2.Stop()
    Write-Host "flush      : SYNCHRONIZE CACHE(10) pass-through, $st, $($sw2.ElapsedMilliseconds) ms"
    if ($st -ne 'ScsiStatus=0x00') { throw "SYNCHRONIZE CACHE failed: $st" }
    Start-Sleep -Milliseconds 500
}

$backHash = Get-Sha256Hex $back
Write-Host "read back  : $Size B sha256=$backHash"
$ok1 = ($backHash -eq $patHash)
Write-Host ("             {0}" -f $(if ($ok1) { 'MATCH initiator read-back' } else { 'MISMATCH initiator read-back' }))
if (-not $ok1) {
    $d = Get-FirstDiff $pat $back
    Write-Host "             first differing byte at offset $d (pattern 0x$('{0:X2}' -f $pat[$d]) vs read 0x$('{0:X2}' -f $back[$d]))"
}

$ok2 = $null
if (-not $NoFileCheck) {
    if (-not (Test-Path $NsFile)) {
        Write-Host "nsfile     : $NsFile not found - skipped"
    } else {
        $f = [System.IO.File]::Open($NsFile, [System.IO.FileMode]::Open,
                                    [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            $null = $f.Seek($Offset, [System.IO.SeekOrigin]::Begin)
            $disk = New-Object byte[] ([int]$Size)
            $got = 0
            while ($got -lt $disk.Length) {
                $n = $f.Read($disk, $got, $disk.Length - $got)
                if ($n -le 0) { throw "nsfile ended after $got byte(s)" }
                $got += $n
            }
        } finally {
            $f.Dispose()
        }
        $diskHash = Get-Sha256Hex $disk
        Write-Host "namespace  : $NsFile [$Offset..$($Offset + $Size - 1)] sha256=$diskHash"
        $ok2 = ($diskHash -eq $patHash)
        Write-Host ("             {0}" -f $(if ($ok2) { 'MATCH backend namespace file' } else { 'MISMATCH backend namespace file' }))
        if (-not $ok2) {
            $d = Get-FirstDiff $pat $disk
            Write-Host "             first differing byte at offset $d (pattern 0x$('{0:X2}' -f $pat[$d]) vs file 0x$('{0:X2}' -f $disk[$d]))"
        }
    }
}

if ($ok1 -and ($ok2 -ne $false)) {
    Write-Host "RESULT     : PASS"
    exit 0
}
Write-Host "RESULT     : FAIL"
exit 1
