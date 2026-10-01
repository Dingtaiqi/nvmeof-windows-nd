# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
param(
    [string]$serverIp = "192.168.100.2",
    [string]$clientLocalIp = "192.168.100.3",
    [int]$port = 4420,
    [string]$subnqn = "nqn.2024-01.local.rdma:windows-nd",
    [string]$hostnqn = "nqn.2014-08.org.nvmexpress:uuid:ndvmeof-f5-0001",
    # Reuse a key instead of generating one, so a Linux peer can be configured with
    # the SAME string (that is what interop means here).
    [string]$AuthKey = "",
    [string]$CtrlKey = "",
    [int]$DhGroup = 2048,
    [switch]$KeepKeyFile
)
# ===========================================================================
#  DH-HMAC-CHAP over RDMA: our host against our target, five cases.
#
#  The self-test in f5_interop.exe -authselftest proves the state machines agree
#  with the reference concatenations.  It does NOT prove that either of them works
#  over a queue pair: the payloads there are handed from one struct to the next in
#  one process, and the two things that can only fail on the wire - the RDMA
#  Read/Write of the payload through the SGL, and the refusal gate that has to let
#  Auth Send/Receive through while blocking everything else - are simply absent.
#
#  So this runs the real thing, and every case asserts the OUTCOME:
#
#   1. key on both sides            -> authenticated, and a normal admin command
#                                      (Property Get CAP) succeeds afterwards
#   2. host with no key             -> the target refuses it, and the host says so
#   3. host with the WRONG key      -> Failure1 with rescode_exp 0x01
#   4. controller key on both sides -> Success2 is sent and the target records it
#   5. controller key WRONG on the
#      target                       -> the HOST rejects the target
#
#  Case 5 is the one that is easy to leave out and the one that matters most: an
#  implementation that never checks the controller's response passes 1-4.
# ===========================================================================
$ErrorActionPreference = "Continue"
$src = $PSScriptRoot            # 脚本自己所在目录：clone 到哪都能跑
$exe = "$src\f5_interop.exe"

if (-not (Test-Path $exe)) { Write-Host "f5_interop.exe is missing - build it first"; exit 1 }

# A key that the target holds and the host presents is ONE value; -genkey prints it
# and re-parses it, so a generator that emits something its own parser rejects is
# caught before the run rather than blamed on the protocol.
if ($AuthKey -eq "") {
    $line = (& $exe -genkey) | Where-Object { $_ -like "DHHC-1:*" } | Select-Object -First 1
    if (-not $line) { Write-Host "could not generate a key"; exit 1 }
    $AuthKey = $line.Trim()
}
if ($CtrlKey -eq "") {
    $line = (& $exe -genkey) | Where-Object { $_ -like "DHHC-1:*" } | Select-Object -First 1
    if (-not $line) { Write-Host "could not generate a controller key"; exit 1 }
    $CtrlKey = $line.Trim()
}
# A second, different host key: "the wrong key" has to be a well-formed key, or the
# test would be checking the parser instead of the HMAC.
$wrongLine = (& $exe -genkey) | Where-Object { $_ -like "DHHC-1:*" } | Select-Object -First 1
$WrongKey = $wrongLine.Trim()

$keyFile = "$src\f5_authkey.txt"
Set-Content -Path $keyFile -Value @($AuthKey, $CtrlKey) -Encoding ASCII
Write-Host "auth key      : $AuthKey"
Write-Host "controller key: $CtrlKey"
Write-Host "(written to $keyFile for the Linux peer)"

$results = @()
function Run-Case {
    param(
        [string]$Name,
        [string[]]$TargetArgs,
        [string[]]$ClientArgs,
        [string[]]$Expect,       # substrings that must appear in the client output
        [string[]]$ExpectTarget, # ...and in the target's
        [int]$TimeoutSec = 90
    )
    Get-Process f5_interop -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    $srvOut = "$src\auth_srv_$Name.txt"
    $cliOut = "$src\auth_cli_$Name.txt"
    Remove-Item $srvOut, $cliOut -ErrorAction SilentlyContinue

    $srv = Start-Process -FilePath $exe -ArgumentList (@("-target", $serverIp, "$port") + $TargetArgs) `
            -PassThru -RedirectStandardOutput $srvOut
    Start-Sleep -Seconds 2
    $cli = Start-Process -FilePath $exe `
            -ArgumentList (@("-initiator", $serverIp, "$port", $clientLocalIp,
                             "-subnqn", $subnqn, "-hostnqn", $hostnqn, "-queues", "1") + $ClientArgs) `
            -PassThru -RedirectStandardOutput $cliOut
    $cliDone = $cli.WaitForExit($TimeoutSec * 1000)
    if (-not $cliDone) { $cli.Kill(); Write-Host "  [INITIATOR HUNG - killed]" }
    if (-not $srv.WaitForExit(20000)) { $srv.Kill() }
    Start-Sleep -Milliseconds 200

    $cliText = (Get-Content $cliOut -Raw) + ""
    $srvText = (Get-Content $srvOut -Raw) + ""
    $ok = $cliDone
    $missing = @()
    # .Contains() and not -like: PowerShell's wildcard matcher reads `[PASS]` as a
    # character class, so a search for "[PASS] Property Get CAP" looks for one
    # character out of {P,A,S} and never matches.  That made a passing run report
    # FAIL - the same class of mistake as asserting on the wrong field.
    #
    # An expectation that starts with '~' is a REGEX.  That exists for the counters
    # whose exact value is a property of the host's command sequence rather than of the
    # target: the gate case asserted `authRefused=13` and broke the moment the
    # initiator learned one more probe.  "Some commands were refused, and no auth
    # traffic happened at all" is the rule; 13 was an observation.
    foreach ($e in $Expect) {
        if ($e.StartsWith('~')) {
            if ($cliText -notmatch $e.Substring(1)) { $ok = $false; $missing += "client ~ $($e.Substring(1))" }
        } elseif (-not $cliText.Contains($e)) { $ok = $false; $missing += "client: '$e'" }
    }
    foreach ($e in $ExpectTarget) {
        if ($e.StartsWith('~')) {
            if ($srvText -notmatch $e.Substring(1)) { $ok = $false; $missing += "target ~ $($e.Substring(1))" }
        } elseif (-not $srvText.Contains($e)) { $ok = $false; $missing += "target: '$e'" }
    }
    $status = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host ("  [{0}] {1}{2}" -f $status, $Name,
                $(if ($ok) { "" } else { " - missing " + ($missing -join "; ") }))
    $script:results += [pscustomobject]@{ Case = $Name; Status = $status; Missing = ($missing -join "; ") }
}

Write-Host ""
Write-Host "===== DH-HMAC-CHAP over RDMA ($clientLocalIp -> $serverIp`:$port), dhgroup $DhGroup"

# 1. the happy path, one-way
Run-Case -Name "1-oneway" `
    -TargetArgs @("-authkey", $AuthKey, "-authdhgroup", "$DhGroup") `
    -ClientArgs @("-authkey", $AuthKey) `
    -Expect @("ATR set", "Auth Receive delivered a Challenge",
              "the target accepted the host's response (Success1)",
              "DH-HMAC-CHAP authentication completed",
              "[PASS] Property Get CAP",
              "initiator failures: 0") `
    -ExpectTarget @("ATR set", "authSends=2 authReceives=2 authRefused=0",
                   "target failures: 0")

# 2. the target requires it and the host has nothing: the host must stop, and the
#    target must record that it never authenticated anything.
Run-Case -Name "2-host-no-key" `
    -TargetArgs @("-authkey", $AuthKey) `
    -ClientArgs @() `
    -Expect @("ATR set", "no -authkey was given") `
    -ExpectTarget @("ATR set", "authSends=0 authReceives=0")

# 3. a well-formed key that is not the target's: Failure1, rescode_exp 0x01
Run-Case -Name "3-wrong-key" `
    -TargetArgs @("-authkey", $AuthKey) `
    -ClientArgs @("-authkey", $WrongKey) `
    -Expect @("Failure1", "rescode_exp=0x01") `
    -ExpectTarget @("auth failed", "the host's response did not verify")

# 4. bidirectional: the target must prove it holds the controller key, the host must
#    verify it and answer with Success2.
Run-Case -Name "4-bidirectional" `
    -TargetArgs @("-authkey", $AuthKey, "-authctrlkey", $CtrlKey) `
    -ClientArgs @("-authkey", $AuthKey, "-authctrlkey", $CtrlKey) `
    -Expect @("Success2 sent (the controller's own response verified)",
              "DH-HMAC-CHAP authentication completed", "initiator failures: 0") `
    -ExpectTarget @("bidirectional", "authSends=3 authReceives=2")

# 5. the target holds the WRONG controller key: only a host that actually checks
#    rvalid can fail here.
Run-Case -Name "5-wrong-ctrlkey" `
    -TargetArgs @("-authkey", $AuthKey, "-authctrlkey", $WrongKey) `
    -ClientArgs @("-authkey", $AuthKey, "-authctrlkey", $CtrlKey) `
    -Expect @("the TARGET's response did not verify") `
    -ExpectTarget @("bidirectional")

# 6. the refusal gate.  -authskip makes our host ignore ATR and send non-fabrics
#    commands without authenticating, which nvmet answers with 0x4191 | DNR.  Without
#    this case the gate is untested from our own host, because our host always does
#    what the bit says - and a target whose gate is missing looks healthy until a
#    host is misconfigured.
#
#    Note which commands ARE answered: Property Get/Set are fabrics commands, and
#    nvmet runs its fabrics dispatcher before the auth check, so CAP/CC/CSTS still
#    work.  Get Log Page (0x02) and Identify (0x06) are refused.  Asserting "CAP is
#    refused" here would be asserting the wrong rule.
Run-Case -Name "6-gate" `
    -TargetArgs @("-authkey", $AuthKey) `
    -ClientArgs @("-authkey", $AuthKey, "-authskip") `
    -Expect @("ATR is set", "sct=1 sc=0x91", "[FAIL] Identify Controller") `
    -ExpectTarget @("opcode 0x06 refused before authentication",
                    "~authSends=0 authReceives=0",
                    "~authRefused=[1-9][0-9]*")

Write-Host ""
$pass = ($results | Where-Object { $_.Status -eq "PASS" }).Count
$total = $results.Count
Write-Host "auth interop: $pass/$total cases passed"
# run_all.ps1 reduces a suite to "[FAIL] lines + non-zero '* failures:' counts +
# RESULT: lines", so this suite has to speak that language as well as its own.
if ($pass -ne $total) {
    $results | Where-Object { $_.Status -ne "PASS" } | ForEach-Object {
        Write-Host ("  [FAIL] {0}: {1}" -f $_.Case, $_.Missing)
    }
    Write-Host "RESULT: FAIL - $($total - $pass) authentication case(s) failed"
    exit 1
}
Write-Host "RESULT: PASS - all $total DH-HMAC-CHAP cases behaved as the reference says"
exit 0
