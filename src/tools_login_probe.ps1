# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
# ===========================================================================
#  tools_login_probe.ps1 - send a REAL login request at a target and dump the
#  reply, byte for byte.
#
#  Why this exists: Windows' initiator will not tell you *what* it disliked about
#  a Login Response - it logs "Target failed to respond in time for a login
#  request" (iScsiPrt 43) and waits out its 30 s timer, whether the answer was
#  wrong or never parsed.  The reference target LIO is the ground truth this
#  project compares against, but Windows can only hold ONE session per target,
#  so an A/B against LIO needs a client that is not Windows.
#
#  This probe is that client.  It replays the byte-for-byte login request that
#  Windows sends at the security stage (captured from our own bridge log) and
#  prints the reply.  Same input bytes, two targets, one diff.
#
#  Usage:  .\tools_login_probe.ps1 -Server 192.168.1.9 -Port 3261 -TargetName <iqn>
# ===========================================================================
param(
    [string]$Server = '192.168.1.9',
    [int]$Port = 3261,
    [string]$TargetName = 'iqn.2024-01.local.rdma:linuxtarget',
    [string]$Initiator = 'iqn.1991-05.com.microsoft:yinshibai',
    [string]$SessionType = 'Normal',      # Windows sends "Normal" for the real session
    [switch]$Stage2,                      # also send the operational login
    [int]$TimeoutMs = 6000
)

function Ascii([string]$s) { [System.Text.Encoding]::ASCII.GetBytes($s) }

# --- the text segment: exactly the four keys Windows offers, NUL-terminated ----
# Order and spelling are copied from the measured request, not from the RFC:
# the point of a byte-for-byte probe is that only the target varies.
$text = "InitiatorName=$Initiator`0" + "SessionType=$SessionType`0" +
        "TargetName=$TargetName`0" + "AuthMethod=None`0"
$textBytes = Ascii $text

# --- the 48-byte BHS, again exactly as Windows builds it ----------------------
$bhs = New-Object byte[] 48
$bhs[0] = 0x43                      # Login Request, immediate
$bhs[1] = 0x81                      # T=1, CSG=0 (security), NSG=1 (login operational)
$bhs[2] = 0x00                      # AHS length
$bhs[3] = 0x00
$bhs[4] = 0x00
$bhs[5] = 0x00
$bhs[6] = [byte](($textBytes.Length -shr 8) -band 0xFF)
$bhs[7] = [byte]($textBytes.Length -band 0xFF)
# bytes 8-13: ISID, bytes 14-15: TSIH (0 = new session) - Windows' own values
@(0x40,0x00,0x01,0x37,0x00,0x00) | ForEach-Object -Begin { $i = 8 } -Process { $bhs[$i] = [byte]$_; $i++ }
$bhs[16] = 0x00; $bhs[17] = 0x00; $bhs[18] = 0x00; $bhs[19] = 0x01   # ITT = 1
$bhs[20] = 0x00; $bhs[21] = 0x01                                     # CID = 1
$bhs[24] = 0x00; $bhs[25] = 0x00; $bhs[26] = 0x00; $bhs[27] = 0x01   # CmdSN = 1
$bhs[28] = 0x00; $bhs[29] = 0x00; $bhs[30] = 0x00; $bhs[31] = 0x01   # ExpStatSN = 1

$pad = (4 - ($textBytes.Length % 4)) % 4
$req = New-Object byte[] (48 + $textBytes.Length + $pad)
[Array]::Copy($bhs, 0, $req, 0, 48)
[Array]::Copy($textBytes, 0, $req, 48, $textBytes.Length)

Write-Host "== $Server`:$Port  target='$TargetName'  sessionType=$SessionType"
Write-Host "   request: BHS 48 + text $($textBytes.Length) + pad $pad = $($req.Length) bytes"

# --- send, then read whole PDUs (BHS, then its data segment + padding) --------
$client = New-Object System.Net.Sockets.TcpClient
try { $client.Connect($Server, $Port) } catch { Write-Host "   CONNECT FAILED: $($_.Exception.Message)"; exit 1 }
$stream = $client.GetStream()
$stream.ReadTimeout = $TimeoutMs
$stream.Write($req, 0, $req.Length)
$stream.Flush()

function Read-Exact($stream, [int]$n, [int]$timeoutMs) {
    $buf = New-Object byte[] $n
    $got = 0
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ($got -lt $n) {
        if ((Get-Date) -gt $deadline) { return ,@($buf, $got, $false) }
        try { $r = $stream.Read($buf, $got, $n - $got) } catch { return ,@($buf, $got, $false) }
        if ($r -le 0) { return ,@($buf, $got, $false) }
        $got += $r
    }
    return ,@($buf, $got, $true)
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$res = Read-Exact $stream 48 $TimeoutMs
$rsp = $res[0]; $have = $res[1]; $complete = $res[2]
if ($have -lt 48) {
    Write-Host "   NO COMPLETE RESPONSE: $have byte(s) after $($sw.ElapsedMilliseconds) ms"
    if ($have -gt 0) {
        for ($i = 0; $i -lt $have; $i++) { Write-Host ("   {0,3}: {1:X2}" -f $i, $rsp[$i]) }
    }
    $client.Close(); exit 0
}

$dl = ($rsp[5] -shl 16) -bor ($rsp[6] -shl 8) -bor $rsp[7]
$padded = $dl + ((4 - ($dl % 4)) % 4)
$data = @()
if ($padded -gt 0) {
    $r2 = Read-Exact $stream $padded $TimeoutMs
    $data = $r2[0]; $dHave = $r2[1]
} else { $dHave = 0 }

Write-Host ("   reply in {0} ms: opcode=0x{1:X2} flags=0x{2:X2} verMax={3} verActive={4} dataLen={5} (read {6} padded)" -f `
    $sw.ElapsedMilliseconds, $rsp[0], $rsp[1], $rsp[2], $rsp[3], $dl, $dHave)
$isid = ($rsp[8..13] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
$tsih = ($rsp[14] -shl 8) -bor $rsp[15]
$itt  = [BitConverter]::ToUInt32(($rsp[16..19])[3..0], 0)
$stat = [BitConverter]::ToUInt32(($rsp[24..27])[3..0], 0)
$exp  = [BitConverter]::ToUInt32(($rsp[28..31])[3..0], 0)
$max  = [BitConverter]::ToUInt32(($rsp[32..35])[3..0], 0)
Write-Host "   T=$([int](($rsp[1] -band 0x80) -ne 0)) CSG=$((($rsp[1] -shr 2) -band 3)) NSG=$($rsp[1] -band 3) ISID=$isid TSIH=$tsih ITT=$itt"
Write-Host "   StatSN=$stat ExpCmdSN=$exp MaxCmdSN=$max statusClass=$($rsp[36]) statusDetail=$($rsp[37])"
Write-Host "   BHS:"
for ($i = 0; $i -lt 48; $i += 16) {
    $line = ($rsp[$i..($i+15)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
    Write-Host ("     {0,3}: {1}" -f $i, $line)
}
if ($dl -gt 0) {
    $txt = [System.Text.Encoding]::ASCII.GetString($data, 0, $dl)
    Write-Host "   text ($dl bytes, dots are NUL):"
    Write-Host ("     " + ($txt -replace "`0", '.'))
    # Byte-exact, because "LIO Target" and "LIO Target " print almost the same and
    # the difference between them is exactly the kind of thing being bisected.
    Write-Host "   text hex:"
    for ($i = 0; $i -lt $dl; $i += 16) {
        $end = [Math]::Min($i + 15, $dl - 1)
        $line = ($data[$i..$end] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
        Write-Host ("     {0,4}: {1}" -f $i, $line)
    }
    Write-Host "   last 4 bytes: $(($data[($dl-4)..($dl-1)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')"
}

# ---------------------------------------------------------------------------
#  Stage 2, the operational login.  Byte-for-byte what Windows sends after a
#  security-stage response (captured from the LIO tap, 168 bytes): T=1, CSG=1,
#  NSG=3, the same ISID, TSIH 0, ITT 1, and ExpStatSN = the StatSN it just got
#  plus one.  This exists because stage 1 alone cannot tell "the target accepted
#  the security stage" from "the target answered and the initiator stopped" -
#  only the second exchange shows whether a session was actually created.
if ($Stage2 -and $have -ge 48) {
    $text2 = "HeaderDigest=None,CRC32C`0" + "DataDigest=None,CRC32C`0" +
             "MaxRecvDataSegmentLength=65536`0" + "DefaultTime2Wait=0`0" +
             "DefaultTime2Retain=60`0"
    $t2 = Ascii $text2
    if ($t2.Length -ne 120) { Write-Host "   STAGE 2 TEXT IS $($t2.Length) BYTES, expected 120 - refusing to send a wrong probe"; $client.Close(); exit 1 }
    $b2 = New-Object byte[] 48
    $b2[0] = 0x43
    $b2[1] = 0x87                                  # T=1, CSG=1, NSG=3, exactly as measured
    $b2[5] = 0x00; $b2[6] = [byte](($t2.Length -shr 8) -band 0xFF); $b2[7] = [byte]($t2.Length -band 0xFF)
    [Array]::Copy($rsp, 8, $b2, 8, 8)              # ISID echoed, TSIH 0
    $b2[19] = 0x01                                 # ITT = 1
    $b2[21] = 0x01                                 # CID = 1
    $b2[27] = 0x01                                 # CmdSN = 1
    $expNext = ($stat + 1) -band 0xFFFFFFFF
    $b2[28] = [byte](($expNext -shr 24) -band 0xFF); $b2[29] = [byte](($expNext -shr 16) -band 0xFF)
    $b2[30] = [byte](($expNext -shr 8) -band 0xFF);  $b2[31] = [byte]($expNext -band 0xFF)
    $pad2 = (4 - ($t2.Length % 4)) % 4
    $req2 = New-Object byte[] (48 + $t2.Length + $pad2)
    [Array]::Copy($b2, 0, $req2, 0, 48); [Array]::Copy($t2, 0, $req2, 48, $t2.Length)
    Write-Host "   -> stage 2: operational login, $($req2.Length) bytes, ExpStatSN=$expNext"
    $sw2 = [System.Diagnostics.Stopwatch]::StartNew()
    $stream.Write($req2, 0, $req2.Length); $stream.Flush()
    $res2 = Read-Exact $stream 48 $TimeoutMs
    $r2b = $res2[0]; $have2 = $res2[1]
    if ($have2 -lt 48) {
        Write-Host "   <- NO OPERATIONAL RESPONSE: $have2 byte(s) after $($sw2.ElapsedMilliseconds) ms"
    } else {
        $dl2 = ($r2b[5] -shl 16) -bor ($r2b[6] -shl 8) -bor $r2b[7]
        $padDl2 = $dl2 + ((4 - ($dl2 % 4)) % 4)
        $d2 = @(); $got2 = 0
        if ($padDl2 -gt 0) { $rr = Read-Exact $stream $padDl2 $TimeoutMs; $d2 = $rr[0]; $got2 = $rr[1] }
        $tsih2 = ($r2b[14] -shl 8) -bor $r2b[15]
        $stat2 = [BitConverter]::ToUInt32(($r2b[24..27])[3..0], 0)
        Write-Host ("   <- reply in {0} ms: opcode=0x{1:X2} flags=0x{2:X2} dataLen={3} TSIH={4} (0 means NO session) StatSN={5}" -f `
            $sw2.ElapsedMilliseconds, $r2b[0], $r2b[1], $dl2, $tsih2, $stat2)
        for ($i = 0; $i -lt 48; $i += 16) {
            $line = ($r2b[$i..($i+15)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
            Write-Host ("     {0,3}: {1}" -f $i, $line)
        }
        if ($dl2 -gt 0) {
            $txt2 = [System.Text.Encoding]::ASCII.GetString($d2, 0, $dl2)
            Write-Host "   text ($dl2 bytes):"
            Write-Host ("     " + ($txt2 -replace "`0", '.'))
        }
        if ($tsih2 -ne 0) {
            Write-Host "   ==> SESSION CREATED (TSIH $tsih2): the target accepted this client."
        } else {
            Write-Host "   ==> NO SESSION: TSIH came back 0."
        }
    }
}
$client.Close()
