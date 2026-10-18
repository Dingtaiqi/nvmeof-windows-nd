# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# Same lesson as boot_4421.ps1: cmd /c start, not Start-Process (see there).
$ErrorActionPreference = 'Continue'
$exe = 'D:\rdma\nvmeof\src\iscsi_tcp_bridge.exe'
Start-Sleep -Seconds 5
if (-not (Get-NetTCPConnection -LocalPort 3260 -State Listen -EA SilentlyContinue)) {
  $line = 'start "" /b "' + $exe + '" -listen 127.0.0.1 3260 -target 127.0.0.1 4421 -lun 0 32768 > D:\rdma\bridge3260.out 2>&1'
  & cmd.exe /c $line
}