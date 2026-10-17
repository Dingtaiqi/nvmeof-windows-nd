# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# netwatch.ps1 - the network watchdog.  Runs every two minutes from a scheduled task.
#
# Twice now I took this machine's network down while experimenting with bridges and adapter
# bindings, and both times the person at the keyboard had to repair it by hand.  And once the
# network is gone I cannot tell what is wrong, because every remote check fails at once.
#
# So this script does two jobs, and it is deliberately short so that it can be read and
# trusted in one sitting:
#
#   1. writes D:\rdma\netwatch.status.txt on every run - whether the gateway, the internet
#      and DNS work, which adapters are up with which addresses, and for how long things
#      have been broken.  That file is readable with no network at all, which is the point.
#   2. if the network has been down for ten minutes, repairs it: restarts the adapters that
#      are already up, and puts the LAN address and default gateway back if they vanished.
#
# What it does NOT do: create, delete or rebind a bridge.  That is what broke the network in
# the first place, so the repair never touches it.

$ErrorActionPreference = 'Continue'

$logFile    = 'D:\rdma\netwatch.log'
$statusFile = 'D:\rdma\netwatch.status.txt'
$stateFile  = 'D:\rdma\netwatch.state.txt'

$gateway    = '192.168.1.1'
$probeIp    = '223.5.5.5'
$probeName  = 'www.microsoft.com'
$lanAddress = '192.168.1.2'
$lanPrefix  = 24
$downLimit  = 600

function Log([string]$m) {
    try { [System.IO.File]::AppendAllText($logFile, ((Get-Date).ToString('yyyy-MM-dd HH:mm:ss') + '  ' + $m + "`r`n")) } catch { }
}

# ---------------------------------------------------------------- load state
$failSince = ''
$resets = 0
if (Test-Path $stateFile) {
    Get-Content $stateFile -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_ -match '^failSince=(.*)$') { $failSince = $matches[1] }
        if ($_ -match '^resets=(\d+)$')   { $resets = [int]$matches[1] }
    }
}
function SaveState {
    $s = 'failSince=' + $failSince + "`r`n" + 'resets=' + $resets + "`r`n"
    try { [System.IO.File]::WriteAllText($stateFile, $s) } catch { }
}

# ---------------------------------------------------------------- probe
$gwOk = [bool](Test-Connection -ComputerName $gateway -Count 2 -Quiet -ErrorAction SilentlyContinue)
$netOk = [bool](Test-Connection -ComputerName $probeIp -Count 2 -Quiet -ErrorAction SilentlyContinue)
$dnsOk = $false
try { $dnsOk = [bool](Resolve-DnsName $probeName -QuickTimeout -ErrorAction Stop | Where-Object { $_.IPAddress }) } catch { $dnsOk = $false }

$healthy = $gwOk -or $netOk

$downFor = 0
if ($failSince -ne '') {
    try { $downFor = [int]((Get-Date) - [datetime]$failSince).TotalSeconds } catch { $downFor = 0 }
}

# ---------------------------------------------------------------- status file
$routeText = ((Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | ForEach-Object { $_.NextHop + ' via ' + $_.InterfaceAlias }) -join '; ')
$adapterText = ((Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' } | ForEach-Object {
    $ips = ((Get-NetIPAddress -InterfaceIndex $_.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | ForEach-Object { $_.IPAddress }) -join ',')
    if ($ips -eq '') { $_.Name + '(no-ip)' } else { $_.Name + '(' + $ips + ')' }
}) -join '  ')

$lines = New-Object System.Collections.ArrayList
[void]$lines.Add('netwatch  ' + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))
[void]$lines.Add('  gateway  ' + $gateway + ' : ' + $gwOk)
[void]$lines.Add('  internet ' + $probeIp + ' : ' + $netOk)
[void]$lines.Add('  dns      ' + $probeName + ' : ' + $dnsOk)
if ($healthy) { [void]$lines.Add('  verdict: UP') } else { [void]$lines.Add('  verdict: DOWN for ' + $downFor + ' s') }
[void]$lines.Add('  default route: ' + $routeText)
[void]$lines.Add('  up adapters: ' + $adapterText)
[void]$lines.Add('  automatic resets so far: ' + $resets)
try { [System.IO.File]::WriteAllText($statusFile, (($lines -join "`r`n") + "`r`n")) } catch { }

# ---------------------------------------------------------------- verdict
if ($healthy) {
    if ($failSince -ne '') { Log ('recovered after ' + $downFor + ' s') }
    $failSince = ''
    SaveState
    exit 0
}

if ($failSince -eq '') {
    $failSince = (Get-Date).ToString('o')
    SaveState
    Log 'network DOWN - ten-minute clock started'
    exit 0
}

Log ('network still DOWN (' + $downFor + ' s)')
if ($downFor -lt $downLimit) { exit 0 }

# ---------------------------------------------------------------- repair
Log '=== ten minutes down: repairing ==='
$resets = $resets + 1

$upAdapters = @(Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' })
foreach ($a in $upAdapters) {
    Log ('  restart adapter ' + $a.Name)
    try { Restart-NetAdapter -Name $a.Name -Confirm:$false -ErrorAction Stop } catch { Log ('    ' + $_.Exception.Message) }
}
Start-Sleep -Seconds 8

$route = Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Select-Object -First 1
$targetIdx = -1
if ($route) {
    $targetIdx = $route.ifIndex
} else {
    $wired = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' } | Select-Object -First 1
    if ($wired) { $targetIdx = $wired.ifIndex }
}

if ($targetIdx -ge 0) {
    $have = @(Get-NetIPAddress -InterfaceIndex $targetIdx -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -eq $lanAddress })
    if ($have.Count -eq 0) {
        Log ('  address ' + $lanAddress + ' missing on ifIndex ' + $targetIdx + ' - putting it back')
        try { New-NetIPAddress -InterfaceIndex $targetIdx -IPAddress $lanAddress -PrefixLength $lanPrefix -DefaultGateway $gateway -ErrorAction Stop | Out-Null } catch { Log ('    ' + $_.Exception.Message) }
    } else {
        Log ('  address is present on ifIndex ' + $targetIdx + ' - leaving it alone')
    }
    try { Set-DnsClientServerAddress -InterfaceIndex $targetIdx -ServerAddresses @($gateway, '223.5.5.5') -ErrorAction SilentlyContinue } catch { }
} else {
    Log '  no adapter is up at all - nothing to repair from here'
}
Start-Sleep -Seconds 6

$gw2 = [bool](Test-Connection -ComputerName $gateway -Count 2 -Quiet -ErrorAction SilentlyContinue)
$net2 = [bool](Test-Connection -ComputerName $probeIp -Count 2 -Quiet -ErrorAction SilentlyContinue)
if ($gw2 -or $net2) {
    Log ('  repair worked (gateway=' + $gw2 + ' internet=' + $net2 + ')')
    $failSince = ''
} else {
    Log '  repair did not work - will try again in ten minutes; a bridge is never touched'
    $failSince = (Get-Date).ToString('o')
}
SaveState
