# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  interop_link.ps1 - put a LAN machine on the same L2 segment as a CX3 port,
#  so that a software-RoCE (rxe) peer can talk to our ND/RoCE endpoint.
#
#  WHY THIS EXISTS
#  RoCEv2 is not routable here: the peer must be in the same subnet, reachable by
#  ARP.  This machine's RoCE endpoints live on the two CX3 ports (192.168.100.2/3)
#  which are cabled to each other, and the LAN (Intel NIC, 192.168.1.2) is a
#  different segment.  Bridging the LAN NIC with ONE CX3 port joins the two
#  segments through the direct cable, leaving the OTHER CX3 port free for the
#  RoCE endpoint - which is exactly one port per endpoint, as a real deployment
#  would be.
#
#  IT ALSO FIXES THE MTU, WHICH WOULD OTHERWISE BREAK THE RUN
#  The CX3 ports are configured for jumbo (IP MTU 4074, RoCE max frame 2048) while
#  the LAN - and therefore the path to the peer - is 1500.  A 2048-byte RoCE frame
#  cannot cross a 1500-byte path: it is not "slow", it is dropped, and the symptom
#  is a connect that times out with nothing in any log.  -LowMtu lowers this side to
#  1500/1024 for the interop run; -RestoreMtu puts the measured-fast settings back.
#
#  WHAT A BRIDGE DOES TO THE TWO NICS IT TAKES - READ THIS BEFORE PANICKING
#  A bridge member does not keep its own protocol stack: Windows unbinds TCP/IP
#  from both member NICs and binds it to the bridge instead.  That is by design,
#  but it makes Windows' own network checker - and anything like it - report
#  "this adapter does not have the TCP/IP service enabled" for those two NICs,
#  even though the segment is perfectly healthy and the BRIDGE is the one holding
#  the address, the DNS servers and (usually) the default route.  Nothing is
#  broken and nothing needs repairing: the bindings come back by themselves the
#  moment the bridge is destroyed, which is exactly what -Down does and verifies.
#  So: while a bridge exists, expect that message about exactly those two NICs.
#
#  EVERY MUTATING ACTION RECORDS STATE FIRST, and -Up rolls itself back if the LAN
#  stops working.  -Down undoes everything from the recorded state.
#
#  Usage:
#    .\interop_link.ps1 -Action Check                 # no changes, just the plan
#    .\interop_link.ps1 -Action Up -PeerIp 192.168.100.5
#    .\interop_link.ps1 -Action Down
#    .\interop_link.ps1 -Action LowMtu
#    .\interop_link.ps1 -Action RestoreMtu
# ===========================================================================
param(
    [ValidateSet('Check', 'Up', 'Down', 'LowMtu', 'RestoreMtu')]
    [string]$Action = 'Check',
    [string]$LanAdapter  = '以太网 20',      # the ordinary LAN NIC
    [string]$RdmaAdapter = '以太网 23',      # CX3 port A - gets bridged
    [string]$KeepAdapter = '以太网 24',      # CX3 port B - stays the RoCE endpoint
    [string]$LanIp       = '192.168.1.2',
    [int]   $PrefixLen   = 24,
    [string]$LanGateway  = '192.168.1.1',
    [string]$PeerIp      = ''                # optional: the peer to ping-verify
)

$ErrorActionPreference = 'Continue'
$stateFile = Join-Path $PSScriptRoot 'interop_link_state.json'

function Say($m)  { Write-Host $m }
function Warn($m) { Write-Host $m -ForegroundColor Yellow }
function Ok($m)   { Write-Host $m -ForegroundColor Green }
function Bad($m)  { Write-Host $m -ForegroundColor Red }

function Get-MtuState {
    $out = @{}
    foreach ($n in @($LanAdapter, $RdmaAdapter, $KeepAdapter)) {
        $ip = Get-NetIPInterface -InterfaceAlias $n -AddressFamily IPv4 -ErrorAction SilentlyContinue
        $adv = Get-NetAdapterAdvancedProperty -Name $n -ErrorAction SilentlyContinue |
               Where-Object { $_.RegistryKeyword -in @('*JumboPacket', 'RoceMaxFrameSize') }
        $out[$n] = @{
            NlMtu   = if ($ip) { $ip.NlMtu } else { $null }
            Dhcp    = if ($ip) { [string]$ip.Dhcp } else { $null }
            # Select-Object -First 1: an adapter can expose the same registry keyword
            # more than once, and then .RegistryValue is an ARRAY - which ConvertTo-Json
            # stores happily and RestoreMtu later cannot write back.
            Jumbo   = ($adv | Where-Object RegistryKeyword -eq '*JumboPacket' | Select-Object -First 1).RegistryValue
            RoceMax = ($adv | Where-Object RegistryKeyword -eq 'RoceMaxFrameSize' | Select-Object -First 1).RegistryValue
        }
    }
    return $out
}

function Read-State {
    # -Encoding UTF8 explicitly.  Without it PowerShell 5.1 reads the file as ANSI,
    # which turns every adapter name into mojibake, matches nothing, and makes a
    # rollback silently restore nothing.  That is not hypothetical: it is what
    # happened to the first state file this script ever wrote.
    return (Get-Content -Raw -Encoding UTF8 $stateFile | ConvertFrom-Json)
}

function Save-State {
    # Keep the previous file, and treat the recorded state as something that can only
    # get better.  `-Action Up` records the pre-bridge state and then calls LowMtu,
    # which used to record the POST-bridge state on top of it: '以太网 20' is a bridge
    # member by then, so it has no address, and the one value a rollback needs was
    # overwritten with emptiness.  Now an empty value never replaces a known one.
    $old = $null
    $hadOld = Test-Path $stateFile
    if ($hadOld) {
        $bak = "$stateFile.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
        Copy-Item $stateFile $bak -Force
        Say "  previous state kept as $bak"
        try { $old = Read-State } catch { Warn "  previous state unreadable ($($_.Exception.Message)); recording fresh" }
    }
    $st = @{
        SavedAt = (Get-Date).ToString('s')
        Mtu     = Get-MtuState
        Ip      = @{}
        Dns     = @{}
        Routes  = @()
    }
    foreach ($n in @($LanAdapter, $RdmaAdapter, $KeepAdapter)) {
        $a = Get-NetIPAddress -InterfaceAlias $n -AddressFamily IPv4 -ErrorAction SilentlyContinue
        $st.Ip[$n] = @($a | ForEach-Object { @{ IPAddress = $_.IPAddress; PrefixLength = $_.PrefixLength } })
        $d = Get-DnsClientServerAddress -InterfaceAlias $n -AddressFamily IPv4 -ErrorAction SilentlyContinue
        $st.Dns[$n] = @(if ($d) { $d.ServerAddresses })
        if ($st.Ip[$n].Count -eq 0 -and $old -and @($old.Ip.$n).Count -gt 0) {
            $st.Ip[$n] = @($old.Ip.$n)
            Warn "  note: '$n' has no address right now; keeping the recorded $((@($old.Ip.$n) | ForEach-Object { $_.IPAddress }) -join ', ')"
        } elseif ($st.Ip[$n].Count -eq 0) {
            Warn "  note: '$n' has no IPv4 address to record (bridged? unplugged?)"
        }
        if ($st.Dns[$n].Count -eq 0 -and $old -and @($old.Dns.$n).Count -gt 0) { $st.Dns[$n] = @($old.Dns.$n) }
        if ($old) {
            foreach ($k in @('NlMtu', 'Jumbo', 'RoceMax', 'Dhcp')) {
                if (-not $st.Mtu[$n].$k -and $old.Mtu.$n.$k) { $st.Mtu[$n].$k = $old.Mtu.$n.$k }
            }
        }
    }
    # Do not record OUR OWN lowered MTU as the machine's baseline - the same rule as
    # the bridge routes below, and it was missing on the MTU half.
    #
    # '-Action LowMtu' sets NlMtu 1500 on BOTH the bridged port and the RoCE endpoint
    # (and Jumbo 1514 / RoCE frame 1024 on the endpoint).  Every call to Save-State
    # while the path is in that state records the artifact as the configuration to
    # return to - and there are several callers: LowMtu itself, and the NEXT window's
    # '-Action Up' if the previous one ended without restoring.  The baseline then
    # ratchets downwards one window at a time until every 4096-byte transfer over the
    # direct link times out with ND_IO_TIMEOUT, which is how run_all went from 8/8 to
    # 2/9 twice in two days.
    #
    # A guard inside LowMtu cannot fix it: the damaging write is the one made by a later
    # '-Action Up', which has no idea the path is still lowered.  The rule lives here,
    # where every caller passes through it.
    if ($old) {
        foreach ($n in @($RdmaAdapter, $KeepAdapter)) {
            $m = $st.Mtu[$n]
            if ($null -eq $m -or $null -eq $old.Mtu.$n) { continue }
            $prev = $old.Mtu.$n
            # The signature is NlMtu 1500 alone.  Keying on all three settings missed
            # the bridged port, which never gets the Jumbo/RoCE-frame pair - so it kept
            # its lowered MTU in the record while the endpoint was protected.
            if ($m.NlMtu -eq 1500 -and $prev.NlMtu -ne 1500) {
                Warn ("  note: '$n' NlMtu is 1500 - this script's own lowered interop setting, " +
                      "not the machine's.  Keeping the recorded " +
                      "$($prev.NlMtu)/$([string]($prev.Jumbo -join ','))/$([string]($prev.RoceMax -join ','))")
                $st.Mtu[$n] = @{ NlMtu = $prev.NlMtu; Jumbo = $prev.Jumbo
                                 RoceMax = $prev.RoceMax; Dhcp = $m.Dhcp }
            }
        }
    }
    # Default routes - but not the ones that exist only because of the bridge.  This
    # function is called again by '-Action LowMtu', and by then '-Action Up' has
    # already moved the LAN's default route onto the bridge.  Recording that would
    # overwrite the machine's real state with our own artifact, and '-Action Down'
    # would then find nothing to restore for the LAN adapter - which is exactly what
    # happened on a machine whose wired NIC is the only internet path (DESIGN 8.49).
    $brNow = Get-BridgeAdapter
    $st.Routes = @(Get-NetRoute -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.DestinationPrefix -eq '0.0.0.0/0' -and
                       (-not $brNow -or $_.InterfaceAlias -ne $brNow.Name) } |
        ForEach-Object { @{ InterfaceAlias = $_.InterfaceAlias; NextHop = $_.NextHop; RouteMetric = $_.RouteMetric } })
    if ($old -and @($st.Routes).Count -eq 0 -and @($old.Routes).Count -gt 0) { $st.Routes = @($old.Routes) }
    # WriteAllText with an explicit BOM: Set-Content -Encoding UTF8 writes a BOM in
    # Windows PowerShell but NOT in PowerShell 7, and the reader may be either one.
    $json = $st | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText($stateFile, $json, (New-Object System.Text.UTF8Encoding($true)))
    Ok "  recorded current state to $stateFile"
}

function Show-Plan {
    Say "== adapters"
    Get-NetAdapter | Where-Object { $_.Name -in @($LanAdapter, $RdmaAdapter, $KeepAdapter) } |
        Format-Table ifIndex, Name, Status, LinkSpeed, MacAddress -AutoSize | Out-String | Write-Host
    Say "== addresses"
    Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.InterfaceAlias -in @($LanAdapter, $RdmaAdapter, $KeepAdapter) } |
        Format-Table InterfaceAlias, IPAddress, PrefixLength -AutoSize | Out-String | Write-Host
    Say "== bridges that exist now"
    (netsh bridge list 2>&1 | Out-String).Trim() | Write-Host
    Say "== TCP/IP binding (a bridge member shows False - that is what the bridge does)"
    foreach ($n in @($LanAdapter, $RdmaAdapter, $KeepAdapter)) {
        $b4 = Get-NetAdapterBinding -Name $n -ComponentID ms_tcpip -ErrorAction SilentlyContinue
        "  {0,-10} TCP/IPv4={1,-5} bridge member={2}" -f $n, $(if ($b4) { [string]$b4.Enabled } else { 'n/a' }), (Test-IsBridged $n)
    }
    Say "== MTU now"
    Get-NetIPInterface -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.InterfaceAlias -in @($LanAdapter, $RdmaAdapter, $KeepAdapter) } |
        Format-Table InterfaceAlias, NlMtu -AutoSize | Out-String | Write-Host
    Say ""
    Say "== plan"
    Say "  1. bridge '$LanAdapter' (LAN) with '$RdmaAdapter' (CX3 port A)"
    Say "     -> frames reach '$KeepAdapter' (CX3 port B, the RoCE endpoint) through the direct cable"
    Say "  2. move $LanIp/$PrefixLen and the default route ($LanGateway) onto the bridge"
    Say "  3. lower the RoCE port to a 1500-byte path (-Action LowMtu), so frames are not dropped"
    Say "  4. peer is expected at 192.168.100.5/24 on the same segment"
    Say ""
    Say "  rollback: .\interop_link.ps1 -Action Down   (and -Action RestoreMtu)"
}

function Get-BridgeAdapter {
    $b = Get-NetAdapter -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -match '桥|Bridge' -and $_.Status -ne 'Not Present' }
    return $b | Select-Object -First 1
}

function Test-IsBridged([string]$name) {
    # `netsh bridge show adapter` lists EVERY adapter with an IsBridged column, so a
    # plain substring match on the name reports "bridged" for all of them.  Read the
    # column instead: <ifIndex> {GUID} <name, padded> <IsBridged> <Bridgeable> <Compat>
    foreach ($line in (netsh bridge show adapter 2>&1)) {
        if ($line -match '^\s*\d+\s+\{[0-9A-Fa-f-]+\}\s+(.+?)\s{2,}(Yes|No)\s+(Yes|No)\s') {
            if ($Matches[1].Trim() -eq $name) { return ($Matches[2] -eq 'Yes') }
        }
    }
    return $false
}

function Invoke-Down([switch]$Quiet) {
    if (-not $Quiet) { Say "== removing the bridge" }
    # `netsh bridge destroy` with NO argument prints its usage and still returns 0,
    # so the one-liner that used to live here reported nothing and changed nothing.
    # The command wants the bridge GUID - and the bridge adapter's own InterfaceGuid
    # IS that GUID, so it does not have to be scraped out of netsh's text output.
    $br = Get-BridgeAdapter
    if ($br) {
        # InterfaceGuid already carries braces, so build the string from the bare GUID.
        # Concatenating braces onto it produced '{{...}}', which netsh answers with
        # "Successfully destroyed bridge" and then does nothing - the same silent
        # no-op this function was rewritten to stop trusting.
        $bare = $br.InterfaceGuid.ToString().Trim('{', '}')
        $guid = '{' + $bare.ToUpper() + '}'
        $o = netsh bridge destroy $guid 2>&1
        if (-not $Quiet) { $o | Where-Object { $_ -match '\S' } | ForEach-Object { "  $_" } }
        for ($i = 0; $i -lt 40; $i++) {
            if (-not (Get-BridgeAdapter)) { break }
            Start-Sleep -Milliseconds 500
        }
        if (Get-BridgeAdapter) {
            Bad "  the bridge is STILL there after 'netsh bridge destroy $guid'."
            Bad "  Nothing else is restored; the member NICs keep TCP/IP switched off."
            return
        }
        if (-not $Quiet) { Ok "  bridge destroyed (GUID $guid)" }
    } elseif (-not $Quiet) { Say "  no bridge to remove" }

    # Pulling a NIC out of a bridge hands the protocol bindings back to it.  While
    # the bridge exists, its members have TCP/IP switched OFF - which is correct,
    # but a network checker reads it as "this adapter has no TCP/IP service" and
    # reports a fault.  Verify the bindings came back and say so, because that
    # message is the one that looks alarming and it is the whole point of -Down.
    Start-Sleep -Seconds 2
    $noTcp = @()
    foreach ($n in @($LanAdapter, $RdmaAdapter)) {
        $b = Get-NetAdapterBinding -Name $n -ComponentID ms_tcpip -ErrorAction SilentlyContinue
        if ($b -and -not $b.Enabled) { $noTcp += $n }
    }
    if ($noTcp.Count -eq 0) {
        if (-not $Quiet) { Ok "  TCP/IP is enabled again on '$LanAdapter' and '$RdmaAdapter'" }
    } else {
        Bad "  TCP/IP is still switched off on: $($noTcp -join ', ')"
    }

    # The address can only go back NOW.  While the NIC was a bridge member,
    # New-NetIPAddress on it failed silently - a bridge member carries no protocol
    # stack of its own - so restoring it before the bridge is gone restores nothing.
    if (Test-Path $stateFile) {
        $st = Read-State
        $want = @($st.Ip.$LanAdapter)
        foreach ($w in $want) {
            $have = Get-NetIPAddress -InterfaceAlias $LanAdapter -AddressFamily IPv4 -ErrorAction SilentlyContinue |
                    Where-Object IPAddress -eq $w.IPAddress
            if (-not $have) {
                New-NetIPAddress -InterfaceAlias $LanAdapter -IPAddress $w.IPAddress `
                                 -PrefixLength $w.PrefixLength -ErrorAction SilentlyContinue | Out-Null
            }
        }
        $dns = $st.Dns.$LanAdapter
        if ($dns -and $dns.Count -gt 0) {
            Set-DnsClientServerAddress -InterfaceAlias $LanAdapter -ServerAddresses $dns -ErrorAction SilentlyContinue
        }
        # Only restore a default route that was actually there before.  If the LAN
        # never carried the default route (this machine's internet used to arrive
        # over the Remote NDIS adapter), adding one here would hijack it.
        #
        # A state file written by the older version can name the BRIDGE instead: the
        # bridge is where '-Action Up' moved the route, and '-Action LowMtu' re-saves
        # the state afterwards - so the machine's real value got overwritten by our
        # own artifact and this restore silently did nothing ("none was recorded for
        # this NIC").  The route still belongs to the LAN adapter, so accept a bridge
        # entry as the record of it.  Save-State no longer writes one (see there).
        $r = $st.Routes | Where-Object { $_.InterfaceAlias -eq $LanAdapter } | Select-Object -First 1
        if (-not $r) {
            $r = $st.Routes | Where-Object { $_.InterfaceAlias -match '桥|Bridge' } | Select-Object -First 1
        }
        if ($r) {
            New-NetRoute -InterfaceAlias $LanAdapter -DestinationPrefix '0.0.0.0/0' `
                         -NextHop $r.NextHop -ErrorAction SilentlyContinue | Out-Null
        }
        # Verify instead of announcing.  Every '-ErrorAction SilentlyContinue' above
        # can fail without a word, and this function used to print "restored ..." no
        # matter what - which is how a rollback that restored nothing went unnoticed.
        $now = @(Get-NetIPAddress -InterfaceAlias $LanAdapter -AddressFamily IPv4 -ErrorAction SilentlyContinue |
                 ForEach-Object { $_.IPAddress })
        $missing = @($want | Where-Object { $_.IPAddress -notin $now })
        $routeNow = @(Get-NetRoute -InterfaceAlias $LanAdapter -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue)
        if (-not $Quiet) {
            if ($missing.Count -eq 0) {
                Ok "  restored on '$LanAdapter': $($now -join ', ')$(if ($dns) { "   dns: $($dns -join ', ')" })"
            } else {
                Bad "  could NOT restore $($missing.IPAddress -join ', ') on '$LanAdapter' (has: $($now -join ', '))"
            }
            if ($r -and $routeNow.Count -eq 0) {
                Bad "  default route: asked to restore via $($r.NextHop) but '$LanAdapter' still has none"
            } else {
                Ok "  default route: $(if ($routeNow.Count) { "via $($routeNow[0].NextHop) (verified present)" }
                                       elseif ($r) { "via $($r.NextHop)" }
                                       else { 'left alone - none was recorded for this NIC' })"
            }
        }
    }
    # ...and the MTU, in the same rollback.  See Restore-Mtu for why it moved here.
    Restore-Mtu -Quiet:$Quiet
}

function Restore-Mtu([switch]$Quiet) {
    # The MTU half of the rollback.  It used to be a SEPARATE action
    # (-Action RestoreMtu) that '-Down' did not call, so the documented sequence was
    # "Down, then RestoreMtu" - and a session that stopped after Down left the RoCE
    # port at 1500/1514/1024.  The next session's '-Action Up' then RECORDED those
    # lowered values as the baseline, and the one after that restored them forever:
    # the local two-port suites fail on every 4096-byte transfer with ND_IO_TIMEOUT.
    # A rollback that rolls back everything except the MTU is a trap, so -Down calls
    # this now, and -Action RestoreMtu remains as the explicit spelling.
    if (-not (Test-Path $stateFile)) { if (-not $Quiet) { Bad "no recorded state; nothing to restore" }; return }
    $st = Read-State
    $now = Get-MtuState          # ONE snapshot, indexed by adapter name
    $changed = @()
    foreach ($n in @($LanAdapter, $RdmaAdapter, $KeepAdapter)) {
        $m = $st.Mtu.$n
        if ($null -eq $m) { continue }
        # Get-MtuState takes NO parameters - it returns a hashtable keyed by adapter -
        # so `Get-MtuState $n` silently returns the whole table and `$cur.NlMtu` is
        # empty.  That made the "-KeepAdapter" guard below and this comparison both
        # read "no value", which is a comparison that can only ever say "different".
        $cur = $now[$n]
        if ($m.NlMtu -and $m.NlMtu -ne $cur.NlMtu) {
            Set-NetIPInterface -InterfaceAlias $n -NlMtuBytes $m.NlMtu -ErrorAction SilentlyContinue
            $changed += "$n NlMtu=$($m.NlMtu)"
        }
        if ($m.RoceMax -and $m.RoceMax -ne $cur.RoceMax) {
            Set-NetAdapterAdvancedProperty -Name $n -RegistryKeyword 'RoceMaxFrameSize' `
                                           -RegistryValue $m.RoceMax -ErrorAction SilentlyContinue | Out-Null
            $changed += "$n RoceMax=$($m.RoceMax)"
        }
        if ($m.Jumbo -and $m.Jumbo -ne $cur.Jumbo) {
            Set-NetAdapterAdvancedProperty -Name $n -RegistryKeyword '*JumboPacket' `
                                           -RegistryValue $m.Jumbo -ErrorAction SilentlyContinue | Out-Null
            $changed += "$n Jumbo=$($m.Jumbo)"
        }
    }
    Start-Sleep -Seconds 2
    if (-not $Quiet) {
        if ($changed.Count) { Ok "  MTU restored: $($changed -join ', ')" }
        else                { Ok "  MTU already at the recorded values" }
    }
    # Verify on the RoCE endpoint, the one whose settings decide whether the interop
    # runs work at all - and the one whose baseline drifted twice.
    $k = (Get-MtuState)[$KeepAdapter]
    $want = $st.Mtu.$KeepAdapter
    if ($want -and $k.NlMtu -ne $want.NlMtu) {
        Bad "  '$KeepAdapter' NlMtu is '$($k.NlMtu)', recorded '$($want.NlMtu)' - NOT restored"
    } elseif (-not $Quiet) {
        Ok "  verified: '$KeepAdapter' NlMtu=$($k.NlMtu) Jumbo=$($k.Jumbo) RoceMax=$($k.RoceMax)"
    }
}

function Test-Lan([string]$peer) {
    # Two probes: a LAN that filters ICMP would make a WORKING bridge look broken,
    # and this function is what decides whether to roll back.
    $gw = Test-Connection $LanGateway -Count 1 -Quiet -ErrorAction SilentlyContinue
    if (-not $gw) {
        $gw = (Test-NetConnection $LanGateway -Port 80 -WarningAction SilentlyContinue).TcpTestSucceeded
    }
    if (-not $gw) { return $false }
    if ($peer) {
        return [bool](Test-Connection $peer -Count 1 -Quiet -ErrorAction SilentlyContinue)
    }
    return $true
}

switch ($Action) {
    'Check' { Show-Plan }

    'LowMtu' {
        # Do NOT record a baseline you are about to overwrite.
        #
        # Save-State stores what a rollback has to restore, and it only ever fills in
        # values that are MISSING - a value that is present always wins.  So calling
        # it while the path is ALREADY lowered records 1500/1514/1024 as the thing to
        # restore, and the baseline then ratchets downwards one window at a time:
        # after a few sessions 'RestoreMtu' faithfully restores the lowered settings
        # and the local two-port suites fail with ND_IO_TIMEOUT on every 4096-byte
        # transfer - which is exactly what happened here (the recorded baseline was
        # 1500/1514/1024 for '以太网 24', while the working configuration for the
        # direct-cable tests is 4074/4088/2048).
        # Check the adapter LowMtu actually LOWERS, and that is the RoCE endpoint
        # ($KeepAdapter), not the bridged port.  Checking the wrong one is how the
        # guard below still let a lowered value be recorded as the baseline: the
        # marker adapter read 4074 while the endpoint had already been at 1500 since
        # a previous session ended without restoring it.
        $cur = (Get-MtuState)[$KeepAdapter]
        if ($cur.NlMtu -ne 1500) {
            Save-State
        } else {
            Say "== the interop path is already at 1500; keeping the recorded baseline"
        }
        Say "== lowering the interop path to 1500 / RoCE 1024"
        Set-NetIPInterface -InterfaceAlias $KeepAdapter -NlMtuBytes 1500 -ErrorAction SilentlyContinue
        Set-NetIPInterface -InterfaceAlias $RdmaAdapter -NlMtuBytes 1500 -ErrorAction SilentlyContinue
        Set-NetAdapterAdvancedProperty -Name $KeepAdapter -RegistryKeyword 'RoceMaxFrameSize' `
                                       -RegistryValue 1024 -ErrorAction SilentlyContinue | Out-Null
        Set-NetAdapterAdvancedProperty -Name $KeepAdapter -RegistryKeyword '*JumboPacket' `
                                       -RegistryValue 1514 -ErrorAction SilentlyContinue | Out-Null
        Start-Sleep -Seconds 2
        Get-NetIPInterface -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.InterfaceAlias -in @($RdmaAdapter, $KeepAdapter) } |
            Format-Table InterfaceAlias, NlMtu -AutoSize | Out-String | Write-Host
        Get-NetAdapterAdvancedProperty -Name $KeepAdapter -ErrorAction SilentlyContinue |
            Where-Object RegistryKeyword -in @('RoceMaxFrameSize', '*JumboPacket') |
            Format-Table DisplayName, DisplayValue -AutoSize | Out-String | Write-Host
    }

    'RestoreMtu' {
        Restore-Mtu
    }

    'Up' {
        Save-State
        $existing = Get-BridgeAdapter
        if ($existing) {
            Warn "  a bridge already exists ('$($existing.Name)'); not creating another"
        } else {
            # Resolve the adapter IDs with Get-NetAdapter, NOT by parsing
            # `netsh bridge show adapter` text.  The ID netsh wants is the
            # interface index, and netsh's output encoding depends on how it is
            # invoked (console vs redirected), which mangled the adapter names
            # when this script ran with -File.  PowerShell's own object output has
            # no such problem.
            $ids = @{}
            Get-NetAdapter | ForEach-Object { $ids[$_.Name] = $_.ifIndex }
            $a = $ids[$LanAdapter]; $b = $ids[$RdmaAdapter]
            if (-not $a -or -not $b) {
                Bad "  could not resolve adapters by name.  Have: $($ids.Keys -join ', ')"
                break
            }
            if (-not (Get-NetAdapter -InterfaceIndex $a).Status -eq 'Up') { Warn "  $LanAdapter is not Up" }
            Say "== creating the bridge: '$LanAdapter' (ifIndex $a) + '$RdmaAdapter' (ifIndex $b)"
            $out = netsh bridge create $a $b 2>&1
            if ($LASTEXITCODE -ne 0) { Bad "  bridge create failed: $out"; break }
            Start-Sleep -Seconds 3
        }

        $br = Get-BridgeAdapter
        if (-not $br) { Bad "  bridge adapter not found; rolling back"; Invoke-Down -Quiet; break }
        Say "  bridge adapter: $($br.Name)"
        # Not a fault, but it is the message a network checker will show, so pre-empt it.
        $off = @()
        foreach ($n in @($LanAdapter, $RdmaAdapter)) {
            $bd = Get-NetAdapterBinding -Name $n -ComponentID ms_tcpip -ErrorAction SilentlyContinue
            if ($bd -and -not $bd.Enabled) { $off += $n }
        }
        if ($off.Count -gt 0) {
            Warn "  expected: TCP/IP is now switched off on $($off -join ' and ') - a bridge member"
            Warn "  hands its protocol stack to the bridge, which is where $LanIp now lives."
            Warn "  Windows' own checker reports this as 'the adapter has no TCP/IP service'."
            Warn "  It is not a fault, and -Action Down hands the bindings back."
        }

        Say "== moving $LanIp/$PrefixLen onto the bridge"
        Remove-NetIPAddress -InterfaceAlias $LanAdapter -AddressFamily IPv4 -Confirm:$false -ErrorAction SilentlyContinue
        New-NetIPAddress -InterfaceAlias $br.Name -IPAddress $LanIp -PrefixLength $PrefixLen -ErrorAction SilentlyContinue | Out-Null
        # DNS has to be set explicitly.  Bridging leaves the bridge with whatever the
        # LAN NIC had only by accident, and when the bridge sat on DHCP/APIPA with a
        # stale resolver configured, name resolution on this machine broke - which is
        # exactly the complaint that produced this line.
        Set-DnsClientServerAddress -InterfaceAlias $br.Name -ServerAddresses $LanGateway -ErrorAction SilentlyContinue
        # The default route is only moved if the LAN adapter actually had it.
        $hadRoute = $false
        if (Test-Path $stateFile) {
            $st = Read-State
            $hadRoute = [bool]($st.Routes | Where-Object { $_.InterfaceAlias -eq $LanAdapter })
        }
        if ($hadRoute) {
            Remove-NetRoute -InterfaceAlias $LanAdapter -DestinationPrefix '0.0.0.0/0' -Confirm:$false -ErrorAction SilentlyContinue
            New-NetRoute -InterfaceAlias $br.Name -DestinationPrefix '0.0.0.0/0' -NextHop $LanGateway -ErrorAction SilentlyContinue | Out-Null
        } else {
            Say "  default route left alone (the LAN did not carry it before)"
        }
        Start-Sleep -Seconds 3

        Say "== verifying the LAN still works"
        # Retry, do not judge on the first probe.  Taking a NIC into a bridge makes it
        # re-initialise - the 1 GbE port drops link and re-negotiates - so a ping sent
        # three seconds later can legitimately fail while the bridge is perfectly fine.
        # The first version of this check rolled the bridge back on that alone.
        $lanOk = $false
        for ($try = 1; $try -le 8; $try++) {
            if (Test-Lan $PeerIp) { $lanOk = $true; break }
            Say "  attempt $try/8: no answer yet, waiting 5 s (the bridged port re-negotiates)"
            Start-Sleep -Seconds 5
        }
        if ($lanOk) {
            Ok "  LAN (and peer) reachable through the bridge"
        } else {
            Bad "  LAN is NOT reachable after 40 s; rolling back automatically"
            Invoke-Down
            break
        }
        Say ""
        Say "next: .\interop_link.ps1 -Action LowMtu   (1500-byte path), then run F5"
        Say "rollback: .\interop_link.ps1 -Action Down"
    }

    'Down' { Invoke-Down }
}
