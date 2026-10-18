# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# verify_after_reboot.ps1 - the acceptance run for the reboot that activates the
# staged target/bridge and the out-of-capsule data path.
#
# Run it once after that reboot.  Every check prints PASS or FAIL and the exit code
# is the number of failures.  It is read-only except for one 64 KiB write/read/delete
# on Z: (and the big-IO selftest, which uses its own temporary namespace on a spare
# port and never touches the live target).

param(
    [string]$DriverHash  = '5BC8D093EC45BCE45914276D2E16C40608044FFFCEAEE532ABE2D5EEB8EF8214',
    [string]$TargetHash  = '0C389764572B6187324F377F44A1216D731103F413C35951759D06094EB80DEE',
    [string]$BridgeHash  = '275E1138894E83BF908CB59AE16885BD3C164C6C5880D68C3430F2587B363F7B',
    [string]$NsImageHash = 'E7180E162618C3FB00ACC3AE0EB1DB619470F5D35125496BC00D307473975DBE',
    [string]$Roundtrip   = '172C15DC2E12B50E523D8E657CBE7FBB11C1053252BBF1E1431077D57D8128FD'
)

$ErrorActionPreference = 'Continue'
$script:fail = 0
function Check([bool]$cond, [string]$what, [string]$detail = '') {
    if ($cond) { Write-Output ('  [PASS] ' + $what) }
    else { $script:fail++; Write-Output ('  [FAIL] ' + $what + $(if ($detail) { '  (' + $detail + ')' } else { '' })) }
}
function HashOf([string]$p) {
    if (-not (Test-Path $p)) { return '(missing)' }
    return (Get-FileHash $p -Algorithm SHA256).Hash
}

Write-Output '=== NVMe-oF acceptance after the staging reboot ==='
Write-Output ('time: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))

Write-Output "`n-- 1. kernel driver --"
$h = HashOf 'C:\Windows\System32\drivers\nvmeofnd2.sys'
Check ($h -eq $DriverHash) 'deployed nvmeofnd2.sys is the signed build' $h
$svc = (sc.exe query nvmeofnd2 2>&1 | Out-String)
Check ($svc -match 'RUNNING') 'service nvmeofnd2 is RUNNING'
$dev = (pnputil /enum-devices /instanceid 'ROOT\HDC\0002' 2>&1 | Out-String)
Check ($dev -match 'Status:\s+Started') 'ROOT\HDC\0002 is Started' (($dev -split "`n" | Where-Object { $_ -match 'Status|Problem' }) -join ' ')

Write-Output "`n-- 2. binaries the boot tasks started --"
$th = HashOf 'D:\rdma\nvmeof\src\nvmeof_tcp.exe'
$bh = HashOf 'D:\rdma\nvmeof\src\iscsi_tcp_bridge.exe'
Check ($th -eq $TargetHash) 'nvmeof_tcp.exe is the out-of-capsule build' $th
Check ($bh -eq $BridgeHash) 'iscsi_tcp_bridge.exe is the 64 KiB build' $bh
Check ((Test-Path 'D:\rdma\nvmeof-ns.img') -and ((Get-Item 'D:\rdma\nvmeof-ns.img').Length -eq 268435456)) 'the namespace image is the 256 MiB one (hash unreadable while the target holds it open)'
$l4421 = (Get-NetTCPConnection -LocalPort 4421 -State Listen -EA SilentlyContinue)
$l3260 = (Get-NetTCPConnection -LocalPort 3260 -State Listen -EA SilentlyContinue)
Check ($null -ne $l4421) 'target is listening on 4421'
Check ($null -ne $l3260) 'bridge is listening on 3260'

Write-Output "`n-- 3. Z: --"
if (-not (Test-Path 'Z:\')) {
    Write-Output '  (Z: is missing; trying the one-shot device restart the boot task uses)'
    & pnputil /restart-device 'ROOT\HDC\0002' 2>&1 | Out-Null
    Start-Sleep -Seconds 5
}
Check (Test-Path 'Z:\') 'Z: is mounted'
Check ((HashOf 'Z:\roundtrip.bin') -eq $Roundtrip) 'roundtrip.bin still hashes the same' (HashOf 'Z:\roundtrip.bin')
$t = 'verify-' + (Get-Date -Format 'HHmmss') + '.bin'
$bytes = New-Object byte[] 65536
(New-Object Random 99).NextBytes($bytes)
$identical = $false
try {
    [System.IO.File]::WriteAllBytes(('Z:\' + $t), $bytes)
    $back = [System.IO.File]::ReadAllBytes(('Z:\' + $t))
    $identical = ($back.Length -eq $bytes.Length)
    if ($identical) {
        for ($i = 0; $i -lt $bytes.Length; $i++) { if ($bytes[$i] -ne $back[$i]) { $identical = $false; break } }
    }
} catch { }
Remove-Item ('Z:\' + $t) -Force -EA SilentlyContinue
Check $identical '64 KiB written to Z: reads back byte for byte'

Write-Output "`n-- 4. out-of-capsule data against the deployed target --"
$selftest = 'D:\rdma\nvmeof\src\tcp_bigio_selftest.exe'
if (Test-Path $selftest) {
    $o = & $selftest 2>&1
    $line = ($o | Where-Object { $_ -match 'pass \d+  fail \d+' } | Select-Object -Last 1)
    Check ($line -match 'fail 0') 'tcp_bigio_selftest: C2HData/H2CData end to end' $line
} else {
    Check $false 'tcp_bigio_selftest.exe exists'
}

Write-Output "`n================================================"
Write-Output ('failures: ' + $script:fail)
exit $script:fail














