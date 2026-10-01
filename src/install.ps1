# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: AGPL-3.0-or-later
#Requires -RunAsAdministrator
<#
    Install the NVMe-oF iSCSI bridge as a Windows service.

    Why a script and not an MSI: an MSI needs WiX (and a download this machine
    cannot make - HTTPS is blocked here), while the whole deliverable is one
    statically-built console exe plus a service registration.  `sc.exe create` with
    the bridge's arguments in the ImagePath is exactly what an MSI would do, and it
    is readable, diffable and uninstallable.

    What it does:
      1. copies f5_interop.exe to %ProgramFiles%\nvmeof-windows-nd\
      2. writes a config file the service reads (bridge.conf, one argument per line)
      3. registers the Windows service with the bridge's command line
      4. sets it to auto-start and starts it
      5. leaves a log at %ProgramData%\nvmeof-windows-nd\bridge.log

    The service runs the iSCSI BRIDGE (-initiator ... -iscsi <port>), i.e. the piece
    Windows' own initiator connects to.  It still needs an NVMe-oF target to talk to
    (the Linux box, or our own -target run); the bridge does not own the namespace.

    Example:
      .\install.ps1 -Target 192.168.100.5 -Subnqn nqn.2024-01.local.rdma:linux-nvmet `
                    -RdmaLocal 192.168.100.3 -Portal 127.0.0.1 -IscsiPort 3260 -ReadWrite

    Uninstall with .\uninstall.ps1
#>
[CmdletBinding()]
param(
    [string] $ServiceName = 'nvmeofNdBridge',
    [string] $InstallDir  = (Join-Path $env:ProgramFiles 'nvmeof-windows-nd'),
    [string] $DataDir     = (Join-Path $env:ProgramData  'nvmeof-windows-nd'),

    # NVMe-oF peer: the target's RDMA address and the subsystem NQN to connect to.
    [Parameter(Mandatory = $true)][string] $Target,
    [string] $Subnqn   = 'nqn.2024-01.local.rdma:linux-nvmet',
    [string] $RdmaPort = '4420',
    # Local RDMA address to connect FROM (the RoCE port on this machine).
    [string] $RdmaLocal = '192.168.100.3',

    # Where Windows' iSCSI initiator will connect, and on which port.
    [string] $IscsiAddr = '127.0.0.1',
    [int]    $IscsiPort = 3260,

    # Writes are refused unless this is set.  nvmet cannot mark a namespace
    # read-only, so the bridge's read-only rail is the only thing keeping the
    # default honest.
    [switch] $ReadWrite,

    # Extra arguments for f5_interop, e.g. '-iscsitrace' while debugging.
    [string[]] $ExtraArgs = @(),

    # Keep retrying the NVMe-oF peer instead of exiting while it is unreachable.  A
    # service whose peer (the Linux box) boots after this machine should come up on its
    # own rather than restart every 5 s until the timing happens to be right.
    [int] $BackendRetrySeconds = 15,

    [string] $ExeSource = (Join-Path $PSScriptRoot 'f5_interop.exe')
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $ExeSource)) {
    throw "f5_interop.exe not found at '$ExeSource'.  Build it first: .\run_f5.ps1 -BuildOnly"
}

$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "service '$ServiceName' already exists; stopping and removing it first"
    if ($svc.Status -ne 'Stopped') { Stop-Service -Name $ServiceName -Force }
    & sc.exe delete $ServiceName | Out-Null
    Start-Sleep -Seconds 2
}

New-Item -ItemType Directory -Force -Path $InstallDir, $DataDir | Out-Null
Copy-Item -Force $ExeSource (Join-Path $InstallDir 'f5_interop.exe')
$exe = Join-Path $InstallDir 'f5_interop.exe'
$log = Join-Path $DataDir 'bridge.log'

# One argument per line: readable, diffable, and no quoting puzzle when the SCM
# hands the ImagePath to CreateProcess.
$args = @(
    '-initiator', $Target, $RdmaPort, $RdmaLocal,
    '-subnqn', $Subnqn,
    '-iscsi', "$IscsiPort",
    '-iscsiaddr', $IscsiAddr,
    '-service', '-svcname', $ServiceName,
    '-log', $log
)
if ($ReadWrite) { $args += '-iscsirw' }
if ($BackendRetrySeconds -gt 0) { $args += @('-backendretry', "$BackendRetrySeconds") }
$args += $ExtraArgs

$conf = Join-Path $InstallDir 'bridge.conf'
$args | Set-Content -Path $conf -Encoding ASCII

# ImagePath carries the whole command line, quoted, because that is what the SCM
# passes to CreateProcess.
$binPath = '"' + $exe + '" ' + (($args | ForEach-Object {
    if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
}) -join ' ')

New-Service -Name $ServiceName -BinaryPathName $binPath -DisplayName 'NVMe-oF iSCSI bridge (NetworkDirect)' -Description "Publishes a remote NVMe-oF namespace to Windows' own iSCSI initiator, so it appears as a normal disk.  Read-only unless installed with -ReadWrite." -StartupType Automatic | Out-Null
# New-Service, not `sc.exe create ... binPath= ...`: PowerShell cannot pass sc.exe's
# "option= value with spaces" form without mangling the quotes, and the failure mode
# is sc printing its usage text while creating nothing - which is what happened here
# first.  The cmdlet takes the binary path as one string, which is what is needed.
# Restart on failure, because a bridge that died silently is a missing disk.
& sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/15000/restart/60000 | Out-Null
Write-Host "installed  : $exe"
Write-Host "config     : $conf"
Write-Host "log        : $log"
Write-Host "service    : $ServiceName ($((Get-Service $ServiceName).StartType))"

Start-Service -Name $ServiceName
Start-Sleep -Seconds 3
$s = Get-Service -Name $ServiceName
Write-Host "started    : $($s.Status)"
if ($s.Status -ne 'Running') {
    Write-Host "--- last log lines ---"
    if (Test-Path $log) { Get-Content $log -Tail 20 | ForEach-Object { Write-Host "  $_" } }
    throw "service did not start"
}
Write-Host ''
Write-Host 'Connect Windows to it with:'
Write-Host "  iscsicli AddTargetPortal $IscsiAddr $IscsiPort"
Write-Host '  Connect-IscsiTarget -NodeAddress iqn.2024-01.com.nvmeof:bridge0 -IsPersistent $false'
