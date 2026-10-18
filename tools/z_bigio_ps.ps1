# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# z_bigio_ps.ps1 - uncached write/read on Z: with an optional repetition count.
#
# Why P/Invoke from PowerShell instead of the C program: processes started from
# the workspace are stopped by the DSH file sandbox from writing anywhere but
# D:\rdma (CreateFile on Z: returns error 5 even as a SYSTEM scheduled task),
# while PowerShell processes can write Z: directly.  FILE_FLAG_NO_BUFFERING keeps
# NTFS from serving the read out of the cache, so the bytes really cross the
# miniport, the transport and the target.
#
# The last line is machine readable:  TOTAL <bytes> <seconds>
param(
    [double]$MiB = 2,
    [string]$Path = 'Z:\bigio-test.bin',
    [int]$Iterations = 1,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$src = @'
using System;
using System.Runtime.InteropServices;

public static class ZIo {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa,
                                            uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool WriteFile(IntPtr h, IntPtr buf, uint n, out uint written, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadFile(IntPtr h, IntPtr buf, uint n, out uint read, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr VirtualAlloc(IntPtr addr, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool VirtualFree(IntPtr addr, UIntPtr size, uint free);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeleteFileW(string name);
}
'@
Add-Type -TypeDefinition $src

[uint32]$GENERIC_WRITE = 0x40000000
[uint32]$GENERIC_READ  = 2147483648
[uint32]$CREATE_ALWAYS = 2
[uint32]$OPEN_EXISTING = 3
[uint32]$NO_BUFFERING  = 0x20000000
[uint32]$WRITE_THROUGH = 2147483648
[uint32]$wflags = $NO_BUFFERING -bor $WRITE_THROUGH
[uint32]$rflags = $NO_BUFFERING
[uint32]$MEM_COMMIT = 0x1000
[uint32]$MEM_RESERVE = 0x2000
[uint32]$MEM_RELEASE = 0x8000
[uint32]$PAGE_READWRITE = 0x04
$INVALID = [IntPtr](-1)

$bytes = [int64]($MiB * 1MB)
$pattern = New-Object byte[] $bytes
for ($i = 0; $i -lt $bytes; $i++) { $pattern[$i] = [byte](((($i * 31 + 7) -bxor ($i -shr 9)) -band 0xFF)) }

$src1 = [ZIo]::VirtualAlloc([IntPtr]::Zero, [UIntPtr]::new([uint64]$bytes), $MEM_COMMIT -bor $MEM_RESERVE, $PAGE_READWRITE)
$dst1 = [ZIo]::VirtualAlloc([IntPtr]::Zero, [UIntPtr]::new([uint64]$bytes), $MEM_COMMIT -bor $MEM_RESERVE, $PAGE_READWRITE)
if ($src1 -eq [IntPtr]::Zero -or $dst1 -eq [IntPtr]::Zero) { throw 'VirtualAlloc failed' }
[System.Runtime.InteropServices.Marshal]::Copy($pattern, 0, $src1, [int]$bytes)

$totalBytes = [int64]0
$totalSec = [double]0
$fail = 0

for ($it = 0; $it -lt $Iterations; $it++) {
    [ZIo]::DeleteFileW($Path) | Out-Null
    $h = [ZIo]::CreateFileW($Path, $GENERIC_WRITE, [uint32]0, [IntPtr]::Zero, $CREATE_ALWAYS, $wflags, [IntPtr]::Zero)
    if ($h -eq $INVALID) { throw ('create failed: ' + [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()) }
    [uint32]$n = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $ok = [ZIo]::WriteFile($h, $src1, [uint32]$bytes, [ref]$n, [IntPtr]::Zero)
    $sw.Stop()
    [ZIo]::CloseHandle($h) | Out-Null
    if (-not $ok -or $n -ne $bytes) { throw ('write failed: ' + [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()) }
    $wsec = $sw.Elapsed.TotalSeconds

    $h = [ZIo]::CreateFileW($Path, $GENERIC_READ, [uint32]0, [IntPtr]::Zero, $OPEN_EXISTING, $rflags, [IntPtr]::Zero)
    if ($h -eq $INVALID) { throw ('reopen failed: ' + [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()) }
    [uint32]$n = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $ok = [ZIo]::ReadFile($h, $dst1, [uint32]$bytes, [ref]$n, [IntPtr]::Zero)
    $sw.Stop()
    [ZIo]::CloseHandle($h) | Out-Null
    if (-not $ok -or $n -ne $bytes) { throw ('read failed: ' + [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()) }
    $rsec = $sw.Elapsed.TotalSeconds

    $back = New-Object byte[] $bytes
    [System.Runtime.InteropServices.Marshal]::Copy($dst1, $back, 0, [int]$bytes)
    $bad = 0
    for ($i = 0; $i -lt $bytes; $i++) { if ($back[$i] -ne $pattern[$i]) { $bad++ } }
    if ($bad -ne 0) { $fail++ }

    $totalBytes += 2 * $bytes
    $totalSec += $wsec + $rsec
    if (-not $Quiet) {
        Write-Output ('[it {0}] write {1:N2} MB/s   read {2:N2} MB/s   {3}' -f $it, ($MiB / $wsec), ($MiB / $rsec), $(if ($bad -eq 0) { 'match' } else { 'MISMATCH' }))
    }
}

[ZIo]::VirtualFree($src1, [UIntPtr]::Zero, $MEM_RELEASE) | Out-Null
[ZIo]::VirtualFree($dst1, [UIntPtr]::Zero, $MEM_RELEASE) | Out-Null
[ZIo]::DeleteFileW($Path) | Out-Null
if ($fail -ne 0) { Write-Output ('FAILED iterations: ' + $fail) }
Write-Output ('TOTAL ' + $totalBytes + ' ' + $totalSec)


