# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# 2026-10-18: two fixes learned the hard way.
# 1) The namespace image moved from D:\nvmeof\bridge-ns.img to D:\rdma\nvmeof-ns.img:
#    D:\nvmeof is protected by the machine's security software (Huorong) and the
#    unsigned nvmeof_tcp.exe gets EACCES from fopen there (even when run as SYSTEM).
# 2) The target is started with `cmd /c start /b ... > log 2>&1`, NOT with
#    PowerShell Start-Process -RedirectStandardOutput -WindowStyle Hidden.
#    Measured 2026-10-18 09:22 on this machine: under SYSTEM in a scheduled task,
#    Start-Process with redirects prints nothing, leaves no process and no listener,
#    while the identical cmd start runs and listens.  The old script used
#    Start-Process, which is why the target was never up after a reboot.
$ErrorActionPreference = 'Continue'
$p   = 'D:\rdma\nvmeof-ns.img'
$exe = 'D:\rdma\nvmeof\src\nvmeof_tcp.exe'
if (-not (Test-Path $p)) { New-Item -ItemType Directory -Path 'D:\rdma' -Force | Out-Null }
if (-not (Get-NetTCPConnection -LocalPort 4421 -State Listen -EA SilentlyContinue)) {
  $line = 'start "" /b "' + $exe + '" -target 127.0.0.1 4421 -nsfile "' + $p + '" -blocks 32768 -serve > D:\rdma\target4421.out 2>&1'
  & cmd.exe /c $line
}