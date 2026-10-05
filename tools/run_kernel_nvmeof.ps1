# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# run_kernel_nvmeof.ps1 - the kernel NVMe-oF exchange, one command, after the reboot.
#
# Where this stands: the kernel driver's whole NDK bring-up works (adapter, PD, CQ, QP,
# connector, MR and a registered region with tokens - all logged with status 0).  What
# does not work yet is the RDMA-CM connection, for a reason that is now understood:
#
#   * the driver used to connect from 192.168.100.2 to 192.168.100.2, i.e. the port to
#     itself.  The CM refuses that (0xC0000236), correctly.
#   * the fix is the dual-address layout D0 used: target on 192.168.100.2 (proven - the
#     user-mode target opens its adapter there and arms its listener), initiator from
#     192.168.100.3 on the card's other port, 以太网 23.
#   * 192.168.100.3 was added with New-NetIPAddress at runtime, and the RDMA stack only
#     enumerates addresses at boot, so NdOpenAdapter on it returns 0xC0000141 and the
#     kernel's open on that port returns 0xC0010011.  A reboot makes the address live.
#   * the reboot also clears the adapters leaked by six deliberate no-teardown test
#     loads, which may additionally have exhausted the provider.
#
# So: reboot first, then run this.

param(
    [string]$Driver  = 'D:\rdma\nvmeof\src\driver\sym\nvmeofk10.sys',
    [string]$F5      = 'D:\rdma\nvmeof\src\f5_interop.exe',
    [string]$NsFile  = 'D:\nvmeof\ns48.img',
    [string]$TgtAddr = '192.168.100.2',
    [string]$IniAddr = '192.168.100.3',
    [int]   $Port    = 4420
)

$mark = 'D:\rdma\run_kernel_nvmeof.txt'
function Mark($m) { [System.IO.File]::AppendAllText($mark, ((Get-Date).ToString('HH:mm:ss.fff') + '  ' + $m + "`r`n")); Write-Output ('  ' + $m) }
function ReadLog {
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('Software\NVMeoFProbe')
    if (-not $key) { return @('(no registry key)') }
    $raw = $key.GetValue('nvmeofk_log', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    if ($raw -is [byte[]]) { $txt = [System.Text.Encoding]::ASCII.GetString($raw) }
    else {
        $str = [string]$raw; $bytes = @()
        foreach ($ch in $str.ToCharArray()) { $c = [int]$ch; $bytes += [byte]($c -band 0xFF); $bytes += [byte](($c -shr 8) -band 0xFF) }
        $txt = [System.Text.Encoding]::ASCII.GetString([byte[]]$bytes)
    }
    $key.Close()
    return ($txt -split "`r?`n" | Where-Object { $_.Trim() })
}
function AddrToNetOrder([string]$ip) {
    $o = $ip.Split('.'); return ([uint32]$o[3] -shl 24) -bor ([uint32]$o[2] -shl 16) -bor ([uint32]$o[1] -shl 8) -bor [uint32]$o[0]
}

Mark '=== KERNEL NVMe-oF: post-reboot run ==='

Mark '--- 1. environment ---'
Mark ('hypervisor present : ' + (Get-CimInstance Win32_ComputerSystem).HypervisorPresent)
Mark ('boot time          : ' + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime)
$iniNic = $null; $tgtNic = $null
foreach ($n in (Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -match 'Mellanox|InfiniBand' })) {
    $ips = (Get-NetIPAddress -InterfaceIndex $n.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | ForEach-Object { $_.IPAddress })
    $rdma = (Get-NetAdapterRdma -Name $n.Name -ErrorAction SilentlyContinue).Enabled
    Mark ('nic ' + $n.Name + ' ifIndex=' + $n.ifIndex + ' ' + $n.Status + ' ' + $n.LinkSpeed + ' ip=[' + ($ips -join ',') + '] rdma=' + $rdma)
    if ($ips -contains $IniAddr) { $iniNic = $n }
    if ($ips -contains $TgtAddr) { $tgtNic = $n }
}
if (-not $iniNic) { Mark ('[X] ' + $IniAddr + ' is not on any RDMA adapter - the address did not survive the reboot'); Mark 'STOP: tell the operator, the initiator needs a live second address'; return }
if (-not $tgtNic) { Mark ('[X] ' + $TgtAddr + ' is not on any RDMA adapter'); return }
Mark ('initiator address ' + $IniAddr + ' on ' + $iniNic.Name + ' ifIndex=' + $iniNic.ifIndex)
Mark ('target    address ' + $TgtAddr + ' on ' + $tgtNic.Name + ' ifIndex=' + $tgtNic.ifIndex)

Mark '--- 2. start the user-mode target on the target address ---'
Get-Process f5_interop -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
$tout = 'D:\rdma\f5_run_out.txt'
Remove-Item $tout -Force -ErrorAction SilentlyContinue
$tgt = Start-Process -FilePath $F5 -ArgumentList @('-target', $TgtAddr, $Port, '-serve', '0', '-runfor', '240', '-nsfile', $NsFile) `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $tout -RedirectStandardError 'D:\rdma\f5_run_err.txt'
Start-Sleep -Seconds 6
if (Test-Path $tout) { Get-Content $tout | Select-Object -First 14 | ForEach-Object { Mark ('TGT| ' + $_) } }
if ($tgt.HasExited) { Mark '[X] the target exited immediately - it could not open its adapter either; nothing to test'; return }

Mark '--- 3. kernel driver: connect from the initiator address ---'
& reg.exe add 'HKLM\Software\NVMeoFProbe' /v nvmeofk_ifindex /t REG_DWORD /d $iniNic.ifIndex /f 2>&1 | Out-Null
& reg.exe add 'HKLM\Software\NVMeoFProbe' /v nvmeofk_local   /t REG_DWORD /d (AddrToNetOrder $IniAddr) /f 2>&1 | Out-Null
& reg.exe add 'HKLM\Software\NVMeoFProbe' /v nvmeofk_target  /t REG_DWORD /d (AddrToNetOrder $TgtAddr) /f 2>&1 | Out-Null
& reg.exe add 'HKLM\Software\NVMeoFProbe' /v nvmeofk_doconnect /t REG_DWORD /d 1 /f 2>&1 | Out-Null
foreach ($v in @('nvmeofk_log','nvmeofk_connect','nvmeofk_done','nvmeofk_lastcq','nvmeofk_rkey','nvmeofk_lkey')) {
    & reg.exe delete 'HKLM\Software\NVMeoFProbe' /v $v /f 2>&1 | Out-Null
}
$svc = 'nvmeofkrun'
$sys = 'D:\rdma\nvmeof\src\driver\sym\nvmeofkrun.sys'
Copy-Item $Driver $sys -Force
& 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe' sign /fd sha256 /s My /n 'NVMeoF Test Driver' $sys 2>&1 | Select-String 'Successfully' | ForEach-Object { Mark ([string]$_.Line.Trim()) }
& sc.exe delete $svc 2>&1 | Out-Null
& sc.exe create $svc type= kernel start= demand binPath= $sys 2>&1 | Out-Null
Mark ('>>> sc start ' + $svc + '   (ifIndex=' + $iniNic.ifIndex + ' local=' + $IniAddr + ' target=' + $TgtAddr + ')')
$s = & sc.exe start $svc 2>&1
Mark ('sc start: ' + (($s | Where-Object { $_ -match 'STATE|FAILED' }) -join ' | '))
foreach ($i in 1..8) {
    Start-Sleep -Seconds 4
    Mark ('poll ' + $i + ': ' + (((& sc.exe query $svc 2>&1) | Where-Object { $_ -match 'STATE' }) -join ' ' -replace '\s+',' '))
}

Mark '--- 4. THE KERNEL LOG ---'
ReadLog | ForEach-Object { [System.IO.File]::AppendAllText($mark, ('  LOG| ' + $_ + "`r`n")); Write-Output ('  LOG| ' + $_) }
foreach ($v in @('nvmeofk_connect','nvmeofk_done','nvmeofk_lastcq','nvmeofk_rkey','nvmeofk_lkey')) {
    $x = (Get-ItemProperty 'HKLM:\Software\NVMeoFProbe' -Name $v -EA SilentlyContinue).$v
    if ($null -ne $x) { Mark ('VAL| ' + $v + ' = 0x' + ('{0:X8}' -f $x)) }
}

Mark '--- 5. what the target saw ---'
if (Test-Path $tout) { Get-Content $tout | Select-Object -Last 30 | ForEach-Object { Mark ('TGT| ' + $_) } }
if (-not $tgt.HasExited) { Stop-Process -Id $tgt.Id -Force -ErrorAction SilentlyContinue }
Mark '=== RUN DONE ==='
