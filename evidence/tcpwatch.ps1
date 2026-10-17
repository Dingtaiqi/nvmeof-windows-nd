# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# tcpwatch.ps1 - sample the loopback connection table while a provoke runs, so
# the number and lifetime of the transport's TCP connections is recorded with
# timestamps instead of guessed from an accept counter after the fact.

param(
    [int]$Seconds = 40,
    [string]$Out = 'D:\rdma\nvmeof\evidence\tcpwatch.txt'
)

Set-Content -Path $Out -Value ('=== tcpwatch start ' + (Get-Date -Format 'HH:mm:ss.fff')) -Encoding ASCII
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline)
{
    $now = Get-Date -Format 'HH:mm:ss.fff'
    $rows = @(Get-NetTCPConnection -LocalPort 4421 -ErrorAction SilentlyContinue)
    if ($rows.Count -eq 0)
    {
        Add-Content -Path $Out -Value ($now + ' none') -Encoding ASCII
    }
    else
    {
        foreach ($r in $rows)
        {
            Add-Content -Path $Out -Value ('{0} {1}:{2}->{3}:{4} {5} pid={6}' -f $now, $r.LocalAddress, $r.LocalPort, $r.RemoteAddress, $r.RemotePort, $r.State, $r.OwningProcess) -Encoding ASCII
        }
    }
    Start-Sleep -Milliseconds 200
}
Add-Content -Path $Out -Value ('=== tcpwatch end ' + (Get-Date -Format 'HH:mm:ss.fff')) -Encoding ASCII
