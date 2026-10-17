# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# fix_test_at_boot.ps1 - the verification run for the "0xC000020D is
# STATUS_CONNECTION_RESET" fix, executed once by a SYSTEM scheduled task after a
# reboot (the only safe way to load a new driver image on this machine: the old
# image is locked while loaded, and nvtcpk_log was non-empty so a device restart
# was not allowed).
#
# It deliberately idles for longer than the target's 60-second SO_RCVTIMEO
# before the first diskpart command, which is the condition that killed the two
# earlier runs, and it idles again before the round-trip write.  Everything it
# does is appended to evidence\fix_test_run.log, because if this session does not
# survive the reboot the evidence still has to.

$ErrorActionPreference = 'Continue'
$ev  = 'D:\rdma\nvmeof\evidence'
$log = "$ev\fix_test_run.log"
$probe = 'HKLM:\Software\NVMeoFProbe'
$devcon = 'C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe'

function L([string]$s)
{
    $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $s)
    Add-Content -Path $log -Value $line -Encoding ASCII
}

function DecodeAscii-RegSz([string]$s)
{
    # The transport's log is ASCII bytes written with REG_SZ, so the registry
    # stores two of them per UTF-16 code unit: low byte then high byte.
    if ($null -eq $s) { return '' }
    $sb = New-Object Text.StringBuilder
    for ($i = 0; $i -lt $s.Length; $i++)
    {
        $v = [int]$s[$i]
        $lo = $v -band 0xFF
        $hi = ($v -shr 8) -band 0xFF
        if ($lo -ne 0) { [void]$sb.Append([char]$lo) }
        if ($hi -ne 0) { [void]$sb.Append([char]$hi) }
    }
    return $sb.ToString()
}

function Get-NvmeofDev
{
    # The instance id is ROOT\HDC\0002; root\nvmeofndi2 is the hardware id that
    # devcon takes, and Get-PnpDevice -InstanceId does not accept it.
    return (Get-PnpDevice -Class HDC -ErrorAction SilentlyContinue |
            Where-Object { $_.FriendlyName -like '*NVMe-oF/TCP*' } |
            Select-Object -First 1)
}

function DumpLogs([string]$suffix)
{
    $p = Get-ItemProperty $probe -ErrorAction SilentlyContinue
    if ($null -ne $p) {
        if ($null -ne $p.nvmeofnd2_log) {
            Set-Content -Path "$ev\fix_${suffix}_nvmeofnd2_log.txt" -Value $p.nvmeofnd2_log -Encoding ASCII
        }
        if ($null -ne $p.nvtcpk_log) {
            Set-Content -Path "$ev\fix_${suffix}_nvtcpk_log.txt" -Value (DecodeAscii-RegSz $p.nvtcpk_log) -Encoding ASCII
        }
    }
}

Set-Content -Path $log -Value ('=== fix test at boot ' + (Get-Date -Format 'yyyy-MM-ddTHH:mm:ss')) -Encoding ASCII

try
{
    # 0. let boot settle
    Start-Sleep -Seconds 5

    # The deploy had to rename the loaded image out of the way; it is free now.
    Get-ChildItem 'C:\Windows\System32\drivers' -Filter 'nvmeofnd2.sys.old-*' -ErrorAction SilentlyContinue |
        ForEach-Object {
            $n = $_.Name
            Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
            L ('removed the renamed old image ' + $n)
        }

    # 1. the target must be listening before anything touches the device
    $listen = { [bool](Get-NetTCPConnection -LocalPort 4421 -State Listen -ErrorAction SilentlyContinue) }
    if (-not (& $listen)) {
        L 'target is not listening; starting it detached'
        Start-Process -FilePath 'D:\rdma\nvmeof\src\nvmeof_tcp.exe' `
            -ArgumentList @('-target','127.0.0.1','4421','-nsfile','D:\nvmeof\bridge-ns.img','-blocks','32768','-serve') `
            -RedirectStandardOutput 'D:\nvmeof\boot4421.log' -RedirectStandardError 'D:\nvmeof\boot4421.err' -WindowStyle Hidden
        for ($i = 0; $i -lt 60; $i++) { if (& $listen) { break }; Start-Sleep -Milliseconds 500 }
    }
    L ('target listening=' + (& $listen))
    if (-not (& $listen)) { L 'FATAL: no target; stopping'; exit 1 }

    # 2. the device.  If its bring-up raced the target it is failed, and a restart
    # is only allowed when no connection is open: a failed NvtcpOpen publishes
    # nothing (the connection joins the list only after a successful handshake),
    # so the log says whether it is safe.
    DumpLogs 'boot'
    $dev = (Get-NvmeofDev)
    L ('device status=' + $dev.Status + ' problem=' + $dev.Problem)
    if ($dev.Status -ne 'OK') {
        # Give PnP time to finish starting the root device before deciding it failed.
        for ($i = 0; $i -lt 60; $i++) {
            Start-Sleep -Seconds 1
            $dev = (Get-NvmeofDev)
            if ($dev.Status -eq 'OK') { break }
        }
        L ('device status after waiting=' + $dev.Status + ' problem=' + $dev.Problem)
    }
    if ($dev.Status -ne 'OK') {
        $bootLog = ''
        if (Test-Path "$ev\fix_boot_nvtcpk_log.txt") { $bootLog = Get-Content "$ev\fix_boot_nvtcpk_log.txt" -Raw }
        if ($bootLog -match 'is open') {
            L 'device is failed but a connection was opened; NOT restarting (0xCE rule)'
            L 'FATAL: a reboot is needed with the target already listening'
            exit 1
        }
        L 'no connection was opened; restarting the device (safe: nothing is pending)'
        & $devcon restart 'root\nvmeofndi2' 2>&1 | ForEach-Object { L ('devcon: ' + $_) }
        Start-Sleep -Seconds 5
        $dev = (Get-NvmeofDev)
        L ('device after restart status=' + $dev.Status + ' problem=' + $dev.Problem)
    }

    # 3. the remote disk
    $disk = $null
    for ($i = 0; $i -lt 30; $i++) {
        $disk = Get-Disk -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -like '*NVMEOFND*' } | Select-Object -First 1
        if ($disk) { break }
        Start-Sleep -Seconds 1
    }
    if (-not $disk) { L 'FATAL: no NVMEOFND disk enumerated'; exit 1 }
    L ('disk number=' + $disk.Number + ' size=' + $disk.Size + ' friendly=' + $disk.FriendlyName + ' operational=' + $disk.OperationalStatus)

    # 4. keep the bring-up log, then clear both logs so the I/O run is what they show
    DumpLogs 'bringup'
    Remove-ItemProperty -Path $probe -Name nvmeofnd2_log -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path $probe -Name nvtcpk_log -ErrorAction SilentlyContinue
    L 'cleared nvmeofnd2_log and nvtcpk_log'

    # 5. the condition that killed the earlier runs: idle past 60 s, then I/O
    L 'idling 80 s so the target closes the idle connection (SO_RCVTIMEO 60000 ms)'
    Start-Sleep -Seconds 80

    $dp = @(
        ('select disk ' + $disk.Number),
        'online disk',
        'clean',
        'convert gpt',
        'create partition primary',
        'format fs=ntfs quick label=NVMEOF-KERNEL',
        'assign letter=Z',
        'exit'
    ) -join "`r`n"
    Set-Content -Path "$ev\dp_fix.txt" -Value $dp -Encoding ASCII
    L 'running diskpart'
    $dpo = & diskpart /s "$ev\dp_fix.txt" 2>&1 | Out-String
    $dprc = $LASTEXITCODE
    Set-Content -Path "$ev\fix_diskpart.txt" -Value $dpo -Encoding ASCII
    L ('diskpart exit=' + $dprc)

    Start-Sleep -Seconds 2
    $vol = Get-Volume -DriveLetter Z -ErrorAction SilentlyContinue
    if ($vol) {
        L ('volume Z: label=' + $vol.FileSystemLabel + ' fs=' + $vol.FileSystem + ' health=' + $vol.HealthStatus + ' size=' + $vol.Size)
    } else {
        L 'volume Z: MISSING'
    }

    # 6. another idle, then a byte-exact round trip through the driver
    L 'idling 80 s again before the round-trip write'
    Start-Sleep -Seconds 80

    $n = 1048576
    $bytes = New-Object 'byte[]' $n
    $rng = New-Object System.Random 20261017
    $rng.NextBytes($bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $memHash = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '')

    if ($vol) {
        try {
            [IO.File]::WriteAllBytes('Z:\roundtrip.bin', $bytes)
            $written = (Get-Item 'Z:\roundtrip.bin').Length
            $back = [IO.File]::ReadAllBytes('Z:\roundtrip.bin')
            $fileHash = [BitConverter]::ToString($sha.ComputeHash($back)).Replace('-', '')
            Set-Content -Path "$ev\roundtrip_readback.bin" -Value $back -Encoding Byte
            L ('roundtrip written=' + $written + ' memHash=' + $memHash)
            L ('roundtrip fileHash=' + $fileHash + ' match=' + ($fileHash -eq $memHash))
        } catch {
            L ('roundtrip FAILED: ' + $_.Exception.Message)
        }
    } else {
        L 'roundtrip skipped: no Z:'
    }

    # 7. the evidence, decoded
    DumpLogs 'final'
    $t = ''
    if (Test-Path "$ev\fix_final_nvtcpk_log.txt") { $t = Get-Content "$ev\fix_final_nvtcpk_log.txt" -Raw }
    $reconnects = ([regex]::Matches($t, 'reconnect')).Count
    $sendFails = ([regex]::Matches($t, 'send of the capsule failed')).Count
    L ('transport log: reconnect mentions=' + $reconnects + ' send-of-capsule failures=' + $sendFails)
    if ($t -match 'irps: alloc=(\d+) free=(\d+) outstanding=(\d+)') {
        L ('irp counters: alloc=' + $matches[1] + ' free=' + $matches[2] + ' outstanding=' + $matches[3])
    } else {
        L 'irp counters: no failure dump in the log (no send failed)'
    }
    if ($t -match 'ops: issued=(\d+) inFlight=(\d+) maxInFlight=(\d+)') {
        L ('op counters: issued=' + $matches[1] + ' maxInFlight=' + $matches[3])
    }
    L '=== test complete ==='
}
catch
{
    L ('FATAL ' + $_.Exception.Message)
}
finally
{
    Unregister-ScheduledTask -TaskName 'nvmeof-fix-test' -Confirm:$false -ErrorAction SilentlyContinue
}
