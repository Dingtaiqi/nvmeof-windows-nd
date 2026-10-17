# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# 2026-10-18: the namespace image moved from D:\nvmeof\bridge-ns.img to
# D:\rdma\nvmeof-ns.img.  D:\nvmeof is protected by the machine's security
# software (Huorong): the signed shell can write there, but the unsigned
# nvmeof_tcp.exe gets EACCES from fopen, so after the 01:23 blue screen the
# target could not open its namespace, the miniport failed with problem 10 and
# Z: disappeared.  D:\rdma is not protected and the same exe opens it fine.
$ErrorActionPreference = 'Continue'
$p = 'D:\rdma\nvmeof-ns.img'
if (-not (Test-Path $p)) { New-Item -ItemType Directory -Path 'D:\rdma' -Force | Out-Null }
if (-not (Get-NetTCPConnection -LocalPort 4421 -State Listen -EA SilentlyContinue)) {
  Start-Process -FilePath 'D:\rdma\nvmeof\src\nvmeof_tcp.exe' -ArgumentList @('-target','127.0.0.1','4421','-nsfile',$p,'-blocks','32768','-serve') -RedirectStandardOutput 'D:\rdma\target4421.out' -RedirectStandardError 'D:\rdma\target4421.err' -WindowStyle Hidden
}