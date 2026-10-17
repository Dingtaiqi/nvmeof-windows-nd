# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# netreset.ps1 - the system-level network reset, in one command.
#
# Windows' own "Network reset" button is, underneath, a winsock reset and an IP stack reset,
# and both of those only take effect after a restart - which is why that button insists on
# one.  This script does the same work, then asks for the restart, and tells you how to
# cancel it.
#
# It is deliberately a separate script rather than something the watchdog fires by itself.
# The watchdog (netwatch.ps1) repairs a broken link by restarting adapters and putting the
# address back; it runs every two minutes and never restarts the machine.  This script *does*
# restart it, so it should be a decision someone makes, not something that happens while a
# document is open on the screen.
#
#   .\netreset.ps1              do the reset, then ask for a restart in 120 s
#   .\netreset.ps1 -NoRestart   do the reset only (the restart is then up to you)
#   .\netreset.ps1 -Cancel      cancel a pending restart
#
# Cancel a pending restart at any time with:   shutdown /a

param(
    [switch]$NoRestart,
    [switch]$Cancel,
    [int]$DelaySeconds = 120
)

$ErrorActionPreference = 'Continue'

$logFile    = 'D:\rdma\netreset.log'
$statusFile = 'D:\rdma\netwatch.status.txt'

function Log([string]$m) {
    $line = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') + '  ' + $m
    try { [System.IO.File]::AppendAllText($logFile, ($line + "`r`n")) } catch { }
    Write-Host $line
}

if ($Cancel) {
    & shutdown.exe /a 2>&1 | ForEach-Object { Log ('  ' + $_) }
    Log 'pending restart cancelled'
    exit 0
}

Log '=== system network reset (the same work Windows'' Network reset does) ==='

# What the button does, in order.  Each is independent: a failure in one is reported and the
# next still runs, because a half-applied reset still usually helps.
Log '  netsh winsock reset'
& netsh.exe winsock reset 2>&1 | ForEach-Object { Log ('    ' + $_) }

Log '  netsh int ip reset'
& netsh.exe int ip reset 2>&1 | ForEach-Object { Log ('    ' + $_) }

Log '  netsh int ipv6 reset'
& netsh.exe int ipv6 reset 2>&1 | ForEach-Object { Log ('    ' + $_) }

Log '  netsh int tcp reset'
& netsh.exe int tcp reset 2>&1 | ForEach-Object { Log ('    ' + $_) }

# Make sure the LAN address and its gateway are in place for after the restart, so the machine
# comes back reachable instead of coming back with no address at all.
$route = Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Select-Object -First 1
$idx = -1
if ($route) {
    $idx = $route.ifIndex
} else {
    $wired = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' } | Select-Object -First 1
    if ($wired) { $idx = $wired.ifIndex }
}
if ($idx -ge 0) {
    $have = @(Get-NetIPAddress -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -eq '192.168.1.2' })
    if ($have.Count -eq 0) {
        Log ('  re-adding 192.168.1.2/24 gw 192.168.1.1 on ifIndex ' + $idx + ' so the machine comes back reachable')
        try { New-NetIPAddress -InterfaceIndex $idx -IPAddress '192.168.1.2' -PrefixLength 24 -DefaultGateway '192.168.1.1' -ErrorAction Stop | Out-Null } catch { Log ('    ' + $_.Exception.Message) }
    }
    try { Set-DnsClientServerAddress -InterfaceIndex $idx -ServerAddresses @('192.168.1.1', '223.5.5.5') -ErrorAction SilentlyContinue } catch { }
}

# Record it where a machine with no network can still be read.
$extra = New-Object System.Collections.ArrayList
Get-Content $statusFile -ErrorAction SilentlyContinue | ForEach-Object { [void]$extra.Add($_) }
[void]$extra.Add('')
[void]$extra.Add('*** system network reset performed at ' + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') + ' ***')
if (-not $NoRestart) { [void]$extra.Add('*** restart requested in ' + $DelaySeconds + ' s - cancel with: shutdown /a ***') }
try { [System.IO.File]::WriteAllText($statusFile, (($extra -join "`r`n") + "`r`n")) } catch { }

if ($NoRestart) {
    Log 'reset done.  The reset only takes effect after a restart: restart when convenient.'
    exit 0
}

Log ('  requesting a restart in ' + $DelaySeconds + ' seconds (cancel with: shutdown /a)')
& shutdown.exe /r /t $DelaySeconds /c 'netreset: system network reset - cancel with shutdown /a' 2>&1 | ForEach-Object { Log ('    ' + $_) }
Log 'done'
