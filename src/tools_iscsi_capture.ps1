# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
param(
    [int]   $Port    = 3260,
    [int]   $Seconds = 25,
    [string]$Out     = ''      # 空 = 写到脚本旁边（clone 到哪都能跑）
)
# 参数默认值里不能可靠地用 $PSScriptRoot，所以在这里补上。
if (-not $Out) { $Out = Join-Path $PSScriptRoot 'iscsi_capture.txt' }
# ===========================================================================
#  tools_iscsi_capture.ps1 - record what the REAL Windows iSCSI initiator sends.
#
#  Why this exists: the login handshake has several bit fields (T / CSG / NSG,
#  version min/max) and text keys whose exact encoding is easy to misremember - and
#  a bridge built on a misremembered constant produces a client that connects to
#  nothing and says nothing.  This project has already paid for that lesson five
#  times (DESIGN 8.29/8.40/8.44).  So: listen, capture, and write the parser from
#  the bytes the actual client sent.
#
#  It is a capture tool, not a target: it answers nothing, so the initiator times
#  out after its first PDU.  That is enough to see the login/text layout, and the
#  refusal on the initiator side is expected.
# ===========================================================================
$ErrorActionPreference = 'Stop'
$lines = New-Object System.Collections.Generic.List[string]

function Say([string]$s) { Write-Host $s; $lines.Add($s) }

function HexDump([byte[]]$b, [int]$off, [int]$len, [int]$max = 256) {
    $n = [math]::Min($len, $max)
    for ($i = 0; $i -lt $n; $i += 16) {
        $slice = $b[($off + $i)..([math]::Min($off + $i + 15, $off + $n - 1))]
        $hex = ($slice | ForEach-Object { $_.ToString('x2') }) -join ' '
        $asc = ($slice | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } }) -join ''
        Say ("    {0,4}: {1,-47}  {2}" -f $i, $hex, $asc)
    }
    if ($len -gt $max) { Say ("    ... $($len - $max) more bytes") }
}

$opNames = @{
    0x00='NOP-Out'; 0x01='SCSI Command'; 0x02='Task Mgmt Req'; 0x03='Login Request';
    0x04='Text Request'; 0x05='SCSI Data-Out'; 0x06='Logout Request'; 0x10='SNACK';
    0x20='NOP-In'; 0x21='SCSI Response'; 0x22='Task Mgmt Rsp'; 0x23='Login Response';
    0x24='Text Response'; 0x25='SCSI Data-In'; 0x26='Logout Response'; 0x31='R2T'; 0x3F='Reject'
}
$scsiNames = @{
    0x00='TEST UNIT READY'; 0x03='REQUEST SENSE'; 0x12='INQUIRY'; 0x1A='MODE SENSE(6)';
    0x1B='START STOP UNIT'; 0x25='READ CAPACITY(10)'; 0x28='READ(10)'; 0x2A='WRITE(10)';
    0x35='SYNCHRONIZE CACHE(10)'; 0x5A='MODE SENSE(10)'; 0x9E='READ CAPACITY(16)';
    0xA0='REPORT LUNS'; 0x88='READ(16)'; 0x8A='WRITE(16)'; 0x91='SYNCHRONIZE CACHE(16)'
}

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
Say "== listening on 127.0.0.1:$Port for up to $Seconds s"
Say ("   (start the initiator now, e.g. New-IscsiTargetPortal -TargetPortalAddress 127.0.0.1)")

$deadline = (Get-Date).AddSeconds($Seconds)
$listener.Server.ReceiveTimeout = 1000
$client = $null
while ((Get-Date) -lt $deadline -and -not $client) {
    try { if ($listener.Pending()) { $client = $listener.AcceptTcpClient() } }
    catch { }
    Start-Sleep -Milliseconds 200
}
if (-not $client) { Say "== no connection within $Seconds s - was the portal added?"; $lines | Set-Content $Out; exit 1 }

$remote = $client.Client.RemoteEndPoint
Say "== connection from $remote"
$client.ReceiveTimeout = 3000
$stream = $client.GetStream()
$buf = New-Object byte[] 262144
$pending = New-Object System.Collections.Generic.List[byte]
$pduNo = 0
$t0 = Get-Date
while (((Get-Date) -lt $deadline) -and $pduNo -lt 40) {
    try { $got = $stream.Read($buf, 0, $buf.Length) } catch { $got = 0 }
    if ($got -le 0) { if ((Get-Date) -gt $deadline) { break }; Start-Sleep -Milliseconds 100; continue }
    for ($i = 0; $i -lt $got; $i++) { $pending.Add($buf[$i]) }

    # Peel complete PDUs out of the stream: 48-byte BHS + AHS + data segment.
    while ($pending.Count -ge 48) {
        $h = $pending.ToArray()
        $dataLen = ($h[5] -shl 16) -bor ($h[6] -shl 8) -bor $h[7]
        $ahs = $h[4] * 4
        $total = 48 + $ahs + $dataLen
        if ($pending.Count -lt $total) { break }
        $pdu = $pending.GetRange(0, $total).ToArray()
        $pending.RemoveRange(0, $total)
        $pduNo++

        $op = $pdu[0] -band 0x3F
        $imm = ($pdu[0] -band 0x40) -ne 0
        $name = $opNames[[int]$op]; if (-not $name) { $name = "opcode 0x{0:x2}" -f $op }
        Say ""
        Say ("-- PDU {0}: {1}  (immediate={2}, dataSegmentLength={3})" -f $pduNo, $name, $imm, $dataLen)
        HexDump $pdu 0 $total 160

        if ($op -eq 0x03 -or $op -eq 0x04) {
            $b1 = $pdu[1]
            Say ("   b1=0x{0:x2}: T={1} CSG={2} NSG={3} verMax={4}   verMin={5}" -f `
                 $b1, (($b1 -band 0x80) -ne 0), (($b1 -shr 5) -band 0x03), (($b1 -shr 2) -band 0x03), ($b1 -band 0x03), ($pdu[2] -band 0x03))
            Say ("   itt=0x{0:x8} cmdsn={1} expstatsn={2}" -f `
                 (($pdu[16] -shl 24) -bor ($pdu[17] -shl 16) -bor ($pdu[18] -shl 8) -bor $pdu[19]), `
                 (($pdu[24] -shl 24) -bor ($pdu[25] -shl 16) -bor ($pdu[26] -shl 8) -bor $pdu[27]), `
                 (($pdu[28] -shl 24) -bor ($pdu[29] -shl 16) -bor ($pdu[30] -shl 8) -bor $pdu[31]))
        }
        if ($op -eq 0x01) {
            $cdb0 = $pdu[32]
            $nm = $scsiNames[[int]$cdb0]; if (-not $nm) { $nm = "opcode 0x{0:x2}" -f $cdb0 }
            $edtl = ($pdu[20] -shl 24) -bor ($pdu[21] -shl 16) -bor ($pdu[22] -shl 8) -bor $pdu[23]
            Say ("   SCSI {0}; EDTL={1}; CDB={2}" -f $nm, $edtl,
                 (($pdu[32..47] | ForEach-Object { $_.ToString('x2') }) -join ' '))
        }
        if ($dataLen -gt 0 -and ($op -eq 0x03 -or $op -eq 0x04)) {
            Say "   text parameters:"
            $txt = [System.Text.Encoding]::ASCII.GetString($pdu, 48 + $ahs, $dataLen)
            foreach ($kv in ($txt -split "`0")) { if ($kv) { Say "     $kv" } }
        }
    }
}
$listener.Stop()
Say ""
Say "== captured $pduNo PDU(s)"
$lines | Set-Content -Encoding UTF8 $Out
Write-Host "written to $Out"
