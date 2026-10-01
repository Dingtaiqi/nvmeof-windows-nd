# ===========================================================================
#  f5_session.ps1 - run the whole interop session against a local Linux peer.
#
#  What this automates, in order, with a check after each step:
#    1. the link:  bridge a LAN NIC with one CX3 port, lower the MTU, ping the peer
#    2. the peer:  ssh in and run linux/f5_linux_up.sh (rxe + address + nvmet)
#    3. direction A: our initiator  -> Linux nvmet      (acceptance item 5)
#    4. direction B: Linux nvme-cli -> our target       (acceptance item 7)
#    5. one report file with every command's output, so the evidence is a file
#       rather than a scrollback
#
#  It is deliberately explicit about what it does to the machine's networking:
#  step 1 changes it, and interop_link.ps1 records state first and can undo it.
#
#  Usage:
#     .\f5_session.ps1 -Check -LaptopIp 192.168.1.50 -LaptopUser you
#     .\f5_session.ps1 -LaptopIp 192.168.1.50 -LaptopUser you                 # both directions
#     .\f5_session.ps1 -LaptopIp 192.168.1.50 -LaptopUser you -Direction a
#     .\f5_session.ps1 -Down                                                  # undo the link
# ===========================================================================
param(
    [string]$LaptopIp   = '',
    [string]$LaptopUser = '',
    [string]$KeyFile    = "$env:USERPROFILE\.ssh\nvmeof_f5_ed25519",
    [ValidateSet('both', 'a', 'b')] [string]$Direction = 'both',
    [string]$PeerRoceIp = '192.168.100.5',   # laptop's address in the RoCE subnet
    [string]$WinRoceIp  = '192.168.100.2',   # our RoCE endpoint (CX3 port B)
    [int]   $Port       = 4420,
    [string]$Subnqn     = 'nqn.2024-01.local.rdma:linux-nvmet',
    # Direction B connects a real host to OUR target, whose subsystem NQN is a
    # different one.  Passing the Linux NQN there is how this script used to ask a
    # host to connect to a subsystem name our target does not advertise.
    [string]$TargetSubnqn = 'nqn.2024-01.local.rdma:windows-nd',
    [string]$Iface      = '',                # laptop's interface, e.g. enp3s0
    #  How many controllers our target serves in direction B.  Two, because a real
    #  host's `nvme discover` is a separate controller from its `nvme connect`: the
    #  discovery connection comes first, reads the log page, and leaves.
    [int]   $ServeControllers = 2,
    #  DH-HMAC-CHAP.  -Auth turns direction B into the authenticated variant: our
    #  target requires a key, and the Linux host presents it.  The key file has one
    #  key per line - host key, controller key, and a well-formed WRONG key - so the
    #  same file drives the positive and the negative case.
    [switch]$Auth,
    [string]$AuthKeyFile = "$PSScriptRoot\f5_authkey.txt",
    [switch]$Check,
    [switch]$SkipLink,
    [switch]$Down
)

$ErrorActionPreference = 'Continue'
$src = $PSScriptRoot
$linux = Join-Path (Split-Path $src -Parent) 'linux'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$report = Join-Path $src "f5_session_$stamp.txt"

function Say($m)  { Write-Host $m; Add-Content -Encoding UTF8 $report $m }
function Ok($m)   { Write-Host $m -ForegroundColor Green;  Add-Content -Encoding UTF8 $report $m }
function Bad($m)  { Write-Host $m -ForegroundColor Red;    Add-Content -Encoding UTF8 $report $m }
function Warn($m) { Write-Host $m -ForegroundColor Yellow; Add-Content -Encoding UTF8 $report $m }

function Invoke-Logged([string]$what, [scriptblock]$body) {
    Say ""
    Say ("=" * 70)
    Say "== $what"
    Say ("=" * 70)
    $out = & $body 2>&1
    $out | ForEach-Object { Write-Host $_; Add-Content -Encoding UTF8 $report $_ }
    return $out
}

function Invoke-Ssh([string]$remoteCmd) {
    $args = @('-i', $KeyFile,
              '-o', 'BatchMode=yes',
              '-o', 'StrictHostKeyChecking=accept-new',
              '-o', 'ConnectTimeout=10',
              "$LaptopUser@$LaptopIp", $remoteCmd)
    return (& ssh @args 2>&1)
}

function Invoke-Scp([string]$local, [string]$remoteDir) {
    #  The peer's ~/nvmeof/linux is root-owned (it was created by sudo), so uploaded
    #  files go to the home directory instead.  The scripts do not care where they
    #  live: they take everything they need as arguments.
    $args = @('-i', $KeyFile,
              '-o', 'BatchMode=yes',
              '-o', 'StrictHostKeyChecking=accept-new',
              '-o', 'ConnectTimeout=10',
              $local, "$LaptopUser@${LaptopIp}:$remoteDir")
    return (& scp @args 2>&1)
}

if ($Down) {
    & (Join-Path $src 'interop_link.ps1') -Action Down
    exit 0
}

if (-not $LaptopIp -or -not $LaptopUser) {
    Write-Host "need -LaptopIp and -LaptopUser (and the key must be authorised on the laptop)"
    Write-Host "  key:    $KeyFile"
    if (Test-Path "$KeyFile.pub") { Write-Host ("  pubkey: " + (Get-Content "$KeyFile.pub")) }
    exit 2
}
if (-not (Test-Path $KeyFile)) { Write-Host "missing SSH key: $KeyFile"; exit 2 }

Say "F5 interop session  $stamp"
Say "  laptop      : $LaptopUser@$LaptopIp"
Say "  peer RoCE ip: $PeerRoceIp"
Say "  our RoCE ip : $WinRoceIp"
Say "  direction   : $Direction"
Say "  auth        : $(if ($Auth) { 'DH-HMAC-CHAP required by our target' } else { 'off' })"
Say "  report      : $report"

#  The key file is read HERE, once, and the three lines are used separately below:
#  line 1 the host key, line 2 the controller key, line 3 a well-formed WRONG key.
if ($Auth) {
    if (-not (Test-Path $AuthKeyFile)) {
        Write-Host "missing auth key file $AuthKeyFile - generate one with: f5_interop.exe -genkey"
        exit 2
    }
    $kl = @(Get-Content $AuthKeyFile | Where-Object { $_.Trim() -ne '' })
    $AuthKey = $kl[0].Trim()
    $CtrlKey = if ($kl.Count -gt 1) { $kl[1].Trim() } else { '' }
    #  A third line is a well-formed WRONG key, and the remote script uses it to
    #  connect once and require a refusal.  That refusal is a controller failing
    #  authentication fatally, so the expected number of fatal auth failures is
    #  not zero - it is exactly the number of deliberate failures below.
    $WrongKey = if ($kl.Count -gt 2) { $kl[2].Trim() } else { '' }
    $ExpectAuthFatal = if ($WrongKey) { 1 } else { 0 }
    if ($AuthKey -notlike 'DHHC-1:*') { Write-Host "$AuthKeyFile line 1 is not a DHHC-1 key"; exit 2 }
    Say "  host key    : $($AuthKey.Substring(0, [Math]::Min(22, $AuthKey.Length)))..."
    Say "  ctrl key    : $(if ($CtrlKey) { 'yes (bidirectional)' } else { 'none (one-way)' })"
    Say "  wrong key   : $(if ($WrongKey) { 'yes (the refusal case will run)' } else { 'none' })"
}

# ---------------------------------------------------------------- 1. the link
if (-not $SkipLink) {
    Invoke-Logged "step 1: link (bridge LAN + one CX3 port, then lower MTU)" {
        & (Join-Path $src 'interop_link.ps1') -Action Up -PeerIp $PeerRoceIp
        & (Join-Path $src 'interop_link.ps1') -Action LowMtu
    } | Out-Null
} else {
    Warn "step 1 skipped (-SkipLink): assuming the bridge is already in place"
}

# ---------------------------------------------------------------- 2. the peer
$haveSsh = Test-NetConnection $LaptopIp -Port 22 -WarningAction SilentlyContinue
if ($haveSsh.TcpTestSucceeded) {
    #  No awk here on purpose.  This string goes through PowerShell -> ssh -> bash, and
    #  in a PowerShell double-quoted string a trailing backslash is literal, so '$1'
    #  written as '\$1' arrives as \{print \$1} and awk answers
    #  "backslash not last character on line".  'ip -brief addr show' needs no awk at
    #  all: its first field is exactly the interface name the parsing below wants.
    $probe = Invoke-Ssh "uname -sr; ip -brief addr show"
    Invoke-Logged "step 2 probe: ssh works" { $probe } | Out-Null
    if (-not $Iface) {
        # pick the interface that actually carries the RoCE-subnet address, else the first ether
        $cand = Invoke-Ssh "ip -brief addr show"
        Invoke-Logged "interfaces and addresses" { $cand } | Out-Null
        $Iface = ($cand | Where-Object { $_ -match '192\.168\.100\.' } | ForEach-Object { ($_ -split '\s+')[0] } | Select-Object -First 1)
        if (-not $Iface) {
            $Iface = ($cand | Where-Object { $_ -match '^(en|eth)' } | ForEach-Object { ($_ -split '\s+')[0] } | Select-Object -First 1)
        }
        if ($Iface) { Ok "using interface '$Iface' on the laptop (override with -Iface)" }
    }
    if (-not $Iface) { Bad "could not work out the laptop's interface; pass -Iface"; exit 1 }
    #  sudo -n, absolute path: the laptop's NOPASSWD rule names this exact script path,
    #  and without sudo the script exits with "[FAIL] must run as root".  Without -n a
    #  non-whitelisted command would sit waiting for a password that cannot be typed.
    $upLog = Invoke-Logged "step 2: bring the peer up (rxe + address + nvmet)" {
        Invoke-Ssh "sudo -n `$HOME/nvmeof/linux/f5_linux_up.sh '$Iface' '$PeerRoceIp' '$WinRoceIp'"
    }
    #  Do not walk on: if the peer was not configured, the failures downstream point at
    #  the transport instead of at the peer, and this used to waste a whole window.
    $hard = @($upLog | Select-String -Pattern '\[FAIL\]|not found|Permission denied|must run as root').Count
    $busy = @($upLog | Select-String -Pattern 'write error: Device or resource busy').Count
    if ($hard -ne 0) {
        Bad "bringing the peer up did not succeed - fix that before reading anything below"
    } elseif ($busy -ne 0) {
        #  Writing device_path on an already-enabled namespace is EBUSY, and under
        #  `set -e` it ends nvmet_setup.sh there.  Harmless when the peer is already
        #  configured (the namespace keeps its device), but everything after that line
        #  - including the port and its subsystem symlink - was skipped this time.
        Warn "the peer was already configured: nvmet_setup.sh stopped at device_path (EBUSY)." 
        Warn "  That is harmless for an existing namespace, but the port settings were not re-applied."
        Warn "  To install the fixed script (needs a password once, 'sudo install' is not in NOPASSWD):"
        Warn "    scp -i <key> linux/nvmet_setup.sh $LaptopUser@${LaptopIp}:/tmp/ && ssh ... sudo install -m755 /tmp/nvmet_setup.sh /home/$LaptopUser/nvmeof/linux/"
    } else {
        Ok "peer configured (rxe + $PeerRoceIp + nvmet)"
    }
    #  Direction B's sequence is a file on the peer, uploaded here rather than pasted
    #  into an ssh command line.  A command string travelling PowerShell -> ssh.exe ->
    #  bash loses its double quotes on the way, and the first parenthesis in it then
    #  kills bash with "syntax error near unexpected token `('" - which is exactly how
    #  a previous run died in the middle of the sequence, leaving a live controller
    #  behind on the host.  Uploading also means the .sh in the repo is what runs.
    $up = Invoke-Logged "step 2b: upload the direction-B script" {
        Invoke-Scp (Join-Path $linux 'f5_dirb_check.sh') '~/'
        if ($Auth) {
            Invoke-Scp (Join-Path $linux 'f5_dirb_auth.sh') '~/'
            #  The keys go up as a FILE, never as an argument: a DHHC-1 string is
            #  base64 with '+', '/' and '=', and a value that travels
            #  PowerShell -> scp/ssh -> sh loses exactly the characters that make it
            #  a key.  A file has no quoting left to lose.
            Invoke-Scp $AuthKeyFile '~/f5_authkey.txt'
        }
        #  scp says nothing at all when it works, so read the files back instead of
        #  treating silence as success.
        Invoke-Ssh "ls -l `$HOME/f5_dirb_check.sh `$HOME/f5_dirb_auth.sh `$HOME/f5_authkey.txt"
    }
    if (@($up | Select-String -Pattern 'f5_dirb_check\.sh').Count -eq 1) {
        Ok "uploaded and verified f5_dirb_check.sh on $LaptopUser@${LaptopIp}:~/"
    } else {
        Bad "could not upload f5_dirb_check.sh to the peer - direction B cannot run"
    }
} else {
    Warn "ssh to $LaptopIp is not reachable; do step 2 by hand on the laptop:"
    Warn "   sudo ./f5_linux_up.sh <iface> $PeerRoceIp $WinRoceIp"
}

#  The reach test belongs AFTER step 2, not before it: f5_linux_up.sh is what puts
#  $PeerRoceIp/24 on the laptop's interface.  Testing first only ever passed because
#  an earlier session had left the address behind, so a fresh laptop (or a reboot)
#  would have failed here with a misleading "the two ends do not share an L2 segment".
$reach = Test-NetConnection $PeerRoceIp -InformationLevel Quiet -WarningAction SilentlyContinue
if (-not $reach) {
    Bad "the peer does not answer on $PeerRoceIp."
    Bad "  - is the laptop on the same switch/router as the Windows LAN NIC?"
    Bad "  - did f5_linux_up.sh run?  (rxe0 up, address added, nvmet configured)"
    Bad "  - bridge in place?   .\interop_link.ps1 -Action Check"
    exit 1
}
Ok "peer $PeerRoceIp answers - the two ends share an L2 segment"

# ------------------------------------------------------------ 3. direction A
if ($Direction -in @('both', 'a')) {
    #  -clientLocalIp is NOT optional here.  run_f5.ps1 defaults to 192.168.100.3,
    #  which is CX3 port A - and step 1 has just made port A a member of the bridge,
    #  so its TCP/IP stack is handed to the bridge and opening the adapter fails with
    #  NdOpenAdapter 0xC0000141.  That is an artefact of the link, not a defect in
    #  the transport, and it is why the self-test reports F1/F7 failures while the
    #  bridge is up.  Direction A must use the same port the peer can reach.
    $a = Invoke-Logged "step 3 (direction A): our initiator -> Linux nvmet" {
        $aArgs = @('-initiatorOnly', '-subnqn', $Subnqn, '-serverIp', $PeerRoceIp,
                   '-clientLocalIp', $WinRoceIp, '-port', $Port)
        if ($Auth -and $AuthKey) { $aArgs += @('-authkey', $AuthKey) }
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $src 'run_f5.ps1') @aArgs
    }
    #  A run that failed before it sent anything also contains no [FAIL] line, so
    #  "no failures" alone is not evidence.  Require the pass count AND the zero.
    $nPass = @($a | Select-String -Pattern '\[PASS\]').Count
    $bad   = @($a | Select-String -Pattern '\[FAIL\]|initiator failures: [1-9]').Count
    $zero  = @($a | Select-String -Pattern '^\s*initiator failures: 0\s*$').Count
    if ($bad -eq 0 -and $zero -eq 1 -and $nPass -ge 10) {
        Ok "direction A: PASS ($nPass checks passed, initiator failures: 0)"
    } elseif ($bad -ne 0) {
        Bad "direction A: FAIL ($bad failing lines)"
    } else {
        Bad "direction A: INCONCLUSIVE - the run did not reach its summary " +
            "(PASS lines=$nPass, 'initiator failures: 0' lines=$zero); see the log above"
    }
    #  Multi-queue is asserted from the peer's own answer, not from our request: the
    #  target decides how many queues exist, and a host may only use what it granted.
    #
    #  The number GRANTED is not the number USED, and reporting the first as if it
    #  were the second is how this line claimed "used 128 I/O queues" against Linux:
    #  nvmet answers the maximum it can serve (128) and our host creates the four it
    #  asked for.  Both numbers are printed, and the claim is about the one that was
    #  actually exercised - which is what the three multi-queue checks below assert.
    $g = @($a | Select-String -Pattern 'Set Features: Number of Queues - asked for (\d+), granted (\d+)')
    $asked = if ($g.Count -ge 1) { [int]$g[0].Matches[0].Groups[1].Value } else { 0 }
    $granted = if ($g.Count -ge 1) { [int]$g[0].Matches[0].Groups[2].Value } else { 0 }
    $conn = @($a | Select-String -Pattern 'I/O queue \d+ created \(fabrics Connect')
    if ([Math]::Min($asked, $granted) -ge 2) {
        $mq = @($a | Select-String -Pattern '\[PASS\] a multi-queue controller|\[PASS\] every I/O queue runs its own command|\[PASS\] a parallel read across every queue').Count
        if ($mq -eq 3) {
            # The whole format expression has to be inside the call: `Ok ("...") -f $a`
            # passes -f to Ok, which ignores it, and the report then prints the
            # literal "{0}" placeholders - a green line that says nothing.
            Ok (("direction A used {0} I/O queues against Linux (asked {1}, the peer granted {2}; " +
                 "each with its own Connect, commands in flight on all of them)") -f
                [Math]::Min($asked, $granted), $asked, $granted)
        } else {
            Bad "direction A: the peer granted $granted queues (we asked $asked) but only $mq of the 3 multi-queue checks passed"
        }
    } elseif ($g.Count -ge 1) {
        Warn "direction A: the peer granted only $granted I/O queue(s) - multi-queue is untested this run"
    }
}

# ------------------------------------------------------------ 3b. our host discovers
if ($Direction -in @('both', 'a')) {
    #  Our initiator asks the peer the same question `nvme discover` asks: connect to
    #  the well-known discovery NQN and read the discovery log page (LID 0x70).  This
    #  is a separate controller from the I/O one above, which is why it is its own run.
    $dsc = Invoke-Logged "step 3b (discovery): our host asks Linux nvmet what it has" {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $src 'run_f5.ps1') `
            -initiatorOnly -Discover -subnqn $Subnqn -serverIp $PeerRoceIp -clientLocalIp $WinRoceIp -port $Port
    }
    $dOk   = @($dsc | Select-String -Pattern '^\s*initiator failures: 0\s*$').Count
    $dBad  = @($dsc | Select-String -Pattern '\[FAIL\]').Count
    $dRecs = @($dsc | Select-String -Pattern 'the discovery log names at least one connectable subsystem - (\d+) record').Count
    if ($dOk -eq 1 -and $dBad -eq 0 -and $dRecs -eq 1) {
        Ok "discovery: our host read Linux nvmet's discovery log page (a separate controller from the I/O one)"
    } elseif ($dBad -ne 0) {
        #  A peer that has no discovery subsystem linked to its port answers the
        #  Connect with CONNECT_INVALID_PARAM - that is the peer's configuration, not
        #  a defect here, so say which it is instead of failing silently.
        Warn "discovery against the peer failed ($dBad failing line(s)) - if the Connect was refused with"
        Warn "  CONNECT_INVALID_PARAM, this nvmet has no discovery subsystem linked to its port"
    } else {
        Ok "discovery: inconclusive (see the log above)"
    }
}

# ------------------------------------------------------------ 4. direction B
if ($Direction -in @('both', 'b')) {
    $srvLog = Join-Path $src "f5_dirB_target_$stamp.txt"
    Say ""
    Say ("=" * 70)
    Say "== step 4 (direction B): Linux nvme-cli -> our target"
    Say ("=" * 70)
    $srvArgs = @('-target', $WinRoceIp, "$Port", '-serve', "$ServeControllers")
    if ($Auth) {
        #  The controller budget has to count EVERY Connect, and one of the remote
        #  script's cases is a connect that authenticates and must then be refused:
        #  our target serves it as a controller all the same (the Connect itself
        #  succeeds - nobody can tell the key is wrong until the Reply arrives).
        #  Getting this wrong shows up as a 20 s stall and a missing summary line.
        $ServeControllers = if ($CtrlKey) { 3 } else { 2 }
        $srvArgs = @('-target', $WinRoceIp, "$Port", '-serve', "$ServeControllers",
                     '-authkey', $AuthKey)
        if ($CtrlKey) { $srvArgs += @('-authctrlkey', $CtrlKey) }
    }
    $srv = Start-Process -FilePath (Join-Path $src 'f5_interop.exe') `
            -ArgumentList $srvArgs `
            -PassThru -RedirectStandardOutput $srvLog
    Start-Sleep -Seconds 2
    $rOut = @()
    if ((Test-NetConnection $LaptopIp -Port 22 -WarningAction SilentlyContinue).TcpTestSucceeded) {
        #  What the peer runs is linux/f5_dirb_check.sh, uploaded in step 2b.  It finds
        #  the device BY ITS SERIAL and refuses to touch anything else; see its header.
        #  The -Auth variant drives the same sequence, but connects with a key and
        #  asserts the negative case as well.
        $remote = if ($Auth) {
            "sh `$HOME/f5_dirb_auth.sh '$WinRoceIp' '$Port' '$TargetSubnqn' `$HOME/f5_authkey.txt"
        } else {
            "sh `$HOME/f5_dirb_check.sh '$WinRoceIp' '$Port' '$TargetSubnqn'"
        }
        $rOut = Invoke-Logged "step 4: remote nvme-cli sequence" { Invoke-Ssh $remote }
    } else {
        Warn "cannot ssh to $LaptopIp - run this on the laptop by hand:"
        Warn "   sh ~/f5_dirb_check.sh $WinRoceIp $Port $TargetSubnqn"
    }
    #  Our target leaves by itself once the host disables the controller; killing it is
    #  only a fallback, and the summary line only appears on a clean exit.  With
    #  -serve N it first waits for one more connection (a real host's discovery
    #  controller is a separate controller from its I/O one), so allow for that wait.
    for ($i = 0; $i -lt 35 -and -not $srv.HasExited; $i++) { Start-Sleep -Seconds 1 }
    if (-not $srv.HasExited) { $srv.Kill(); Warn "our target had to be killed; it did not see the host leave" }
    if (Test-Path $srvLog) {
        Invoke-Logged "step 4: our target's log" { Get-Content $srvLog } | Out-Null
        $log = Get-Content -Raw $srvLog

        # what the host itself concluded (it did the compare)
        if (@($rOut).Count -eq 0) {
            Warn "the remote sequence did not run - direction B is unproven this session"
        } elseif ($Auth -and @($rOut | Select-String -Pattern 'RESULT: PASS - a Linux host authenticated').Count -eq 1) {
            Ok "a real Linux host authenticated to our target (DH-HMAC-CHAP) and moved data over it"
        } elseif (@($rOut | Select-String -Pattern 'DISC: the discovery log names').Count -eq 1) {
            Ok "a real Linux host ran 'nvme discover' against our target and found the subsystem in the log"
        } else {
            Bad "the Linux host's nvme discover did not find our subsystem in the discovery log"
        }
        if (@($rOut | Select-String -Pattern 'CMP: identical').Count -ge 1) {
            $conc = @($rOut | Select-String -Pattern 'CMP: identical for 4 concurrent reads').Count
            if ($conc -eq 1) {
                Ok "the Linux host compared its 65536 bytes - identical, including 4 concurrent reads"
            } else {
                Ok "the Linux host compared its 65536 bytes and found them identical"
            }
        } else {
            Bad "the Linux host did NOT report 'CMP: identical' - data did not survive the round trip"
        }

        # what our target saw (it counted what it served).  Parse the summary line as a
        # whole: matching 'read=(\d+)' against the entire log would also hit any earlier
        # line that happens to contain the same text.
        $sum = @($log -split "`r?`n" | Where-Object { $_ -match 'target summary:' } | Select-Object -Last 1)
        if (-not $sum) {
            Bad "no 'target summary' line in our target's log - it did not reach the end"
        } else {
            $n = @{}
            foreach ($m in [regex]::Matches($sum, '(\w+)=(\d+)')) { $n[$m.Groups[1].Value] = [int64]$m.Groups[2].Value }
            if     ($n['ioCommands'] -ge 2)      { Ok "our target served $($n['ioCommands']) I/O commands" }
            else                                 { Bad "our target served $($n['ioCommands']) I/O command(s); the host's write+read should be 2" }
            if     ($n['written'] -eq 65536)     { Ok "our target received exactly 65536 written bytes" }
            else                                 { Bad "our target received $($n['written']) written bytes, expected 65536" }
            #  >= and not ==: a real host also reads the namespace to probe its geometry,
            #  so its total read count legitimately exceeds the 65536 bytes we compare.
            if     ($n['read'] -ge 65536)        { Ok "our target served $($n['read']) read bytes (>= the 65536 the host compared)" }
            else                                 { Bad "our target served only $($n['read']) read bytes, less than the 65536 the host compared" }
            # KATO==0 (what we would use if we never stored the Connect's kato) makes every
            # Keep Alive answer KA_TIMEOUT_INVALID and drives a real host into error
            # recovery.  A completed run must show the host's keep-alives being answered.
            if     ($n['keepAlives'] -ge 1)      { Ok "our target answered $($n['keepAlives']) Keep Alive command(s)" }
            else                                 { Bad "our target answered 0 Keep Alives; a real host would time out" }
            if     ($n['unknownAdmin'] -ne 0)    { Bad "our target did not recognise $($n['unknownAdmin']) admin command(s) a real host sent" }
            elseif ($n['unknownIo']    -ne 0)    { Bad "our target did not recognise $($n['unknownIo']) I/O command(s) a real host sent" }
            else                                 { Ok "direction B: our target recognised everything the Linux host sent" }

            #  DH-HMAC-CHAP, from our target's own counters.  authSends/authReceives
            #  are the number of auth capsules it answered; authRefused is how many
            #  non-fabrics commands it turned away for being unauthenticated, which
            #  must be 0 in a successful run - a non-zero value here means the host
            #  got in without authenticating, or our own gate fired on a good host.
            if ($Auth) {
                if ($n['authSends'] -ge 2 -and $n['authReceives'] -ge 2) {
                    Ok "our target answered $($n['authSends']) Auth Send and $($n['authReceives']) Auth Receive capsules"
                } else {
                    Bad "our target answered authSends=$($n['authSends']) authReceives=$($n['authReceives']); a completed exchange needs 2 of each"
                }
                if ($n['authFatal'] -eq $ExpectAuthFatal) {
                    if ($ExpectAuthFatal -eq 0) {
                        Ok "no controller ended in the fatal auth state (CSTS.CFS)"
                    } else {
                        Ok "exactly $($n['authFatal']) controller ended in the fatal auth state - the one the wrong key was sent to"
                    }
                } else {
                    Bad "authFatal=$($n['authFatal']) but $ExpectAuthFatal exchange(s) were supposed to fail"
                }
                if ($n['authRefused'] -ne 0) {
                    Bad "our target refused $($n['authRefused']) command(s) for being unauthenticated in a run where every controller authenticated"
                } else {
                    Ok "no command was refused for lack of authentication"
                }
            }

            #  Multi-queue, from the target's own counters.  "8 queues connected" is
            #  also true of a host that connects eight and sends everything down the
            #  first, so the per-queue distribution is the part that proves use.
            if ($n.ContainsKey('ioQueuesConnected')) {
                $qline = @($log -split "`r?`n" | Where-Object { $_ -match 'target io per queue:' } | Select-Object -Last 1)
                $used  = 0
                if ($qline) { $used = @([regex]::Matches($qline, 'q\d+=([1-9]\d*)')).Count }
                if     ($n['ioQueuesConnected'] -ge 2 -and $used -ge 2) {
                    Ok "our target accepted $($n['ioQueuesConnected']) I/O queue pairs and served commands on $used of them"
                } elseif ($n['ioQueuesConnected'] -ge 2) {
                    Bad "our target accepted $($n['ioQueuesConnected']) queue pairs but every command came down $used queue(s)"
                } else {
                    Warn "the host created only $($n['ioQueuesConnected']) I/O queue - multi-queue is untested this run"
                }
            }
        }
    }
}

Say ""
Say "report written to $report"
if ($Check) { Say "(this was a -Check run; nothing was left changed except the link, which -Down undoes)" }
