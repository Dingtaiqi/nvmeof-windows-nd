# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# provoke_cycles.ps1 - repeat the failure condition that the fix targets:
# leave the transport idle for longer than the target's 60 s SO_RCVTIMEO, then
# issue I/O.  Every cycle is a fresh opportunity for the reconnect+resend path,
# and every cycle records the read result, the number of target accepts and the
# transport log's reconnect count, so an intermittent failure cannot hide.

param(
    [int]$Cycles = 4,
    [int]$IdleSeconds = 65,
    [string]$Ev = 'D:\rdma\nvmeof\evidence',
    [string]$Out = 'D:\rdma\nvmeof\evidence\provoke_cycles.txt'
)

Add-Content -Path $Out -Value ('=== provoke cycles ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) -Encoding ASCII

function TargetAccepts
{
    $t = Get-Content 'D:\nvmeof\boot4421.log' -ErrorAction SilentlyContinue
    return ([regex]::Matches(($t -join "`n"), '\[conn \d+\] accepted')).Count
}

function ReconnectCount
{
    $p = Get-ItemProperty 'HKLM:\Software\NVMeoFProbe' -ErrorAction SilentlyContinue
    if ($null -eq $p -or $null -eq $p.nvtcpk_log) { return 0 }
    $s = [string]$p.nvtcpk_log
    $sb = New-Object Text.StringBuilder
    for ($i = 0; $i -lt $s.Length; $i++)
    {
        $v = [int]$s[$i]
        $lo = $v -band 0xFF
        $hi = ($v -shr 8) -band 0xFF
        if ($lo -ne 0) { [void]$sb.Append([char]$lo) }
        if ($hi -ne 0) { [void]$sb.Append([char]$hi) }
    }
    return ([regex]::Matches($sb.ToString(), 'is up again')).Count
}

for ($c = 1; $c -le $Cycles; $c++)
{
    $before = TargetAccepts
    $rcBefore = ReconnectCount
    Add-Content -Path $Out -Value ('[{0}] cycle {1}: idling {2} s (accepts={3} reconnects={4})' -f (Get-Date -Format 'HH:mm:ss'), $c, $IdleSeconds, $before, $rcBefore) -Encoding ASCII
    Start-Sleep -Seconds $IdleSeconds

    $off = 512 * (1000 * $c)
    $child = Start-Process -FilePath 'powershell.exe' `
        -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$Ev\provoke_read.ps1",'-Disk','5','-Offset',"$off",'-Bytes','512','-Tag',"cycle$c") `
        -PassThru -WindowStyle Hidden
    $ok = $child.WaitForExit(20000)
    if (-not $ok) { try { $child.Kill() } catch {} }

    $after = TargetAccepts
    $rcAfter = ReconnectCount
    $last = (Get-Content "$Ev\provoke_read_result.txt" -ErrorAction SilentlyContinue | Select-Object -Last 1)
    Add-Content -Path $Out -Value ('[{0}] cycle {1}: exited={2} accepts {3}->{4} reconnects {5}->{6}' -f (Get-Date -Format 'HH:mm:ss'), $c, $ok, $before, $after, $rcBefore, $rcAfter) -Encoding ASCII
    Add-Content -Path $Out -Value ('    ' + $last) -Encoding ASCII
    if (-not $ok) { Add-Content -Path $Out -Value '    READ HUNG > 20 s: stopping so the evidence can be captured' -Encoding ASCII; break }
    if ($after - $before -gt 1) { Add-Content -Path $Out -Value ('    NOTE: {0} accepts for one reconnect cycle' -f ($after - $before)) -Encoding ASCII }
}
Add-Content -Path $Out -Value '=== provoke cycles done' -Encoding ASCII
