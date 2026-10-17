# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# run_kernel_listener.ps1 - load a fresh copy of nvmeofk_lb.sys and read its log.
#
# This script touches ONLY the nvmeofk* test driver: it never stops, restarts or
# reconfigures nvmeofnd2 / nvmeofndi (they serve Z:), never touches the target or
# bridge processes, and never reboots.
#
# Why a fresh image per run: an NDK-less legacy kernel service is reported by the
# SCM as NOT_STOPPABLE here (`sc stop` -> 1052), so the driver cannot be unloaded
# between runs.  Every run therefore gets its own file name and service name.  The
# previous instance is harmless: its DriverEntry closed every NDK object and
# returned both adapters before it returned (the log proves it), so what stays
# resident is an idle 40 KB image, not provider state.
param(
    [ValidateSet(0, 1)][int]$Connect = 0,
    [string]$Tag = 'listen-only',
    [string]$Instance = (Get-Date -Format 'HHmmss'),
    [int]$Port = 4420
)

$ErrorActionPreference = 'Continue'
$base   = 'D:\rdma\nvmeof\src\driver\sym\k\nvmeofk_d4.sys'
$driver = 'D:\rdma\nvmeof\src\driver\sym\k\nvmeofk_lb_' + $Instance + '.sys'
$svc    = 'nvmeofkloop' + $Instance
$out    = 'D:\rdma\klisten_' + $Tag + '.txt'

function Say([string]$m) {
    $line = (Get-Date).ToString('HH:mm:ss.fff') + '  ' + $m
    [System.IO.File]::AppendAllText($out, $line + "`r`n")
    Write-Output $line
}

function ReadKernelLog {
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('Software\NVMeoFProbe')
    if (-not $key) { return @('(no registry key)') }
    $raw = $key.GetValue('nvmeofk_log', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    if ($null -eq $raw) { $key.Close(); return @('(nvmeofk_log is absent)') }
    # Rule 7: the ASCII log lives inside a REG_SZ, so each UTF-16 unit holds TWO
    # bytes of it (low byte first).  Taking only the low byte loses half the text.
    if ($raw -is [byte[]]) {
        $txt = [System.Text.Encoding]::ASCII.GetString($raw)
    } else {
        $bytes = New-Object System.Collections.Generic.List[byte]
        foreach ($ch in ([string]$raw).ToCharArray()) {
            $c = [int]$ch
            $bytes.Add([byte]($c -band 0xFF))
            $bytes.Add([byte](($c -shr 8) -band 0xFF))
        }
        $txt = [System.Text.Encoding]::ASCII.GetString($bytes.ToArray())
    }
    $key.Close()
    return ($txt -split "`r?`n" | Where-Object { $_.Trim() })
}

function AddrToNetOrder([string]$ip) {
    $o = $ip.Split('.')
    return ([uint32]$o[3] -shl 24) -bor ([uint32]$o[2] -shl 16) -bor ([uint32]$o[1] -shl 8) -bor [uint32]$o[0]
}

if (Test-Path $out) { Remove-Item $out -Force }
Say ('=== nvmeofk listener run: ' + $Tag + '  connect=' + $Connect + '  instance=' + $Instance + ' ===')
Say ('base   : ' + $base)
Say ('driver : ' + $driver)

Copy-Item $base $driver -Force
Say ('sha256 : ' + (Get-FileHash $driver -Algorithm SHA256).Hash)

# --- sign with the test certificate (the same one every driver in this repo uses)
$sig = & 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe' sign /fd sha256 /s My /n 'NVMeoF Test Driver' $driver 2>&1
($sig | Where-Object { $_ -match 'Successfully|Error|failed' }) | ForEach-Object { Say ('sign| ' + $_.ToString().Trim()) }

# --- knobs: listener on .2 (ifIndex 30), initiator on .3 (ifIndex 17)
$knobs = @{
    nvmeofk_dolisten = 1
    nvmeofk_lconnect = $Connect
    nvmeofk_ifindex  = 17
    nvmeofk_lifindex = 30
    nvmeofk_local    = (AddrToNetOrder '192.168.100.3')
    nvmeofk_target   = (AddrToNetOrder '192.168.100.2')
    nvmeofk_lport    = $Port
}
foreach ($k in $knobs.Keys) {
    & reg.exe add 'HKLM\Software\NVMeoFProbe' /v $k /t REG_DWORD /d $knobs[$k] /f 2>&1 | Out-Null
    Say ('reg  : ' + $k + ' = ' + $knobs[$k] + ' (0x' + ('{0:X8}' -f [uint32]$knobs[$k]) + ')')
}
foreach ($v in @('nvmeofk_log','nvmeofk_ok','nvmeofk_connect','nvmeofk_accept','nvmeofk_listen',
                 'nvmeofk_done','nvmeofk_cap','nvmeofk_cap_status','nvmeofk_cntlid',
                 'nvmeofk_connect_status','nvmeofk_lastcq','nvmeofk_llastcq','nvmeofk_rkey','nvmeofk_lkey')) {
    & reg.exe delete 'HKLM\Software\NVMeoFProbe' /v $v /f 2>&1 | Out-Null
}

# --- create and start this instance
& sc.exe delete $svc 2>&1 | Out-Null
Start-Sleep -Milliseconds 300
$c = & sc.exe create $svc type= kernel start= demand binPath= $driver 2>&1
Say ('sc create: ' + (($c | Where-Object { $_ -match 'SUCCESS|FAILED' }) -join ' '))

$t0 = Get-Date
$s = & sc.exe start $svc 2>&1
Say ('sc start: ' + (($s | Where-Object { $_ -match 'STATE|FAILED' }) -join ' | '))
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 1
    $q = (& sc.exe query $svc 2>&1 | Where-Object { $_ -match 'STATE' }) -join ' '
    if ($q -notmatch 'START_PENDING') { break }
}
Say ('sc query: ' + ((& sc.exe query $svc 2>&1 | Where-Object { $_ -match 'STATE' }) -join ' ' -replace '\s+',' '))
Say ('elapsed : ' + [int]((Get-Date) - $t0).TotalSeconds + ' s')

# --- the evidence
Say '--- nvmeofk_log ---'
$log = ReadKernelLog
foreach ($line in $log) { [System.IO.File]::AppendAllText($out, ('  LOG| ' + $line + "`r`n")) }
foreach ($v in @('nvmeofk_ok','nvmeofk_listen','nvmeofk_connect','nvmeofk_accept','nvmeofk_done',
                 'nvmeofk_connect_status','nvmeofk_cntlid','nvmeofk_cap','nvmeofk_cap_status',
                 'nvmeofk_lastcq','nvmeofk_llastcq','nvmeofk_rkey','nvmeofk_lkey')) {
    $x = (Get-ItemProperty 'HKLM:\Software\NVMeoFProbe' -Name $v -ErrorAction SilentlyContinue).$v
    if ($null -ne $x) { Say ('VAL| ' + $v + ' = 0x' + ('{0:X8}' -f [uint32]$x)) }
}
Say ('log lines: ' + $log.Count)
Say ('driver still loaded: ' + [bool](Get-CimInstance Win32_SystemDriver | Where-Object { $_.Name -eq $svc }))

$st = & sc.exe stop $svc 2>&1
Say ('sc stop : ' + (($st | Where-Object { $_ -match 'STATE|FAILED' }) -join ' | '))
Say ('sc delete: ' + ((& sc.exe delete $svc 2>&1) -join ' '))
Say '=== RUN DONE ==='



