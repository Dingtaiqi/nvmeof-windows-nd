# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# dumplogs.ps1 - decode HKLM\Software\NVMeoFProbe\nvtcpk_log and nvmeofnd2_log.
#
# nvtcpk_log is ASCII bytes written into a REG_SZ, so the registry holds two of
# them per UTF-16 code unit (low byte then high byte).  Reading only the low
# byte yields every other character, which is how earlier sessions mis-read it.
# nvmeofnd2_log is a plain REG_SZ.

param(
    [string]$Suffix = 'current',
    [string]$Dir = 'D:\rdma\nvmeof\evidence'
)

function DecodeAscii-RegSz([string]$s)
{
    if ($null -eq $s) { return '' }
    $sb = New-Object Text.StringBuilder
    for ($i = 0; $i -lt $s.Length; $i++)
    {
        $v = [int]$s[$i]
        $lo = $v -band 0xFF
        $hi = ($v -shr 8) -band 0xFF
        if ($lo -ne 0) { [void]$sb.Append([char]$lo) }
        if ($hi -ne 0) { [void]$sb.Append([char]$hi) }
    }
    return $sb.ToString()
}

$p = Get-ItemProperty 'HKLM:\Software\NVMeoFProbe' -ErrorAction SilentlyContinue
if ($null -eq $p)
{
    Write-Output 'no NVMeoFProbe key'
    exit 1
}

$t = ''
if ($null -ne $p.nvtcpk_log)
{
    $t = DecodeAscii-RegSz $p.nvtcpk_log
    Set-Content -Path "$Dir\${Suffix}_nvtcpk_log.txt" -Value $t -Encoding ASCII
}
$m = ''
if ($null -ne $p.nvmeofnd2_log)
{
    $m = [string]$p.nvmeofnd2_log
    Set-Content -Path "$Dir\${Suffix}_nvmeofnd2_log.txt" -Value $m -Encoding ASCII
}

Write-Output ('nvtcpk_log bytes=' + $t.Length + ' lines=' + (($t -split "`n").Count))
Write-Output ('nvmeofnd2_log chars=' + $m.Length)
Write-Output '--- nvtcpk tail ---'
$lines = $t -split "`n"
$from = [Math]::Max(0, $lines.Count - 40)
for ($i = $from; $i -lt $lines.Count; $i++) { Write-Output $lines[$i] }
