# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0

$ErrorActionPreference = 'Continue'
$p = 'D:\nvmeof\bridge-ns.img'
if (-not (Test-Path $p)) { New-Item -ItemType Directory -Path 'D:\nvmeof' -Force | Out-Null }
if (-not (Get-NetTCPConnection -LocalPort 4421 -State Listen -EA SilentlyContinue)) {
  Start-Process -FilePath 'D:\rdma\nvmeof\src\nvmeof_tcp.exe' -ArgumentList @('-target','127.0.0.1','4421','-nsfile',$p,'-blocks','32768','-serve') -RedirectStandardOutput 'D:\nvmeof\boot4421.log' -RedirectStandardError 'D:\nvmeof\boot4421.err' -WindowStyle Hidden
}