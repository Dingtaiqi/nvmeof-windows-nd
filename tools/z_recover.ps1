# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# z_recover.ps1 - one job, once per boot: if the NVMe-oF/TCP miniport lost the
# race with its target and Z: is absent, restart the device once.
#
# Why this is needed: the StorPort miniport's FindAdapter connects to
# 127.0.0.1:4421 while the device starts, and the boot tasks that start the
# target run later.  When it loses that race the device comes up as problem 10
# (CM_PROB_FAILED_START) and Z: never appears - observed 2026-10-18 01:23 and
# 06:53.  This task does nothing when Z: is already present.
$ErrorActionPreference = 'Continue'
Start-Sleep -Seconds 60
$log = 'D:\rdma\z_recover.log'
if (Test-Path Z:\) { return }
(Get-Date -Format o) + ' Z: missing, restarting ROOT\HDC\0002' | Out-File $log -Append -Encoding ASCII
& pnputil /restart-device 'ROOT\HDC\0002' 2>&1 | Out-File $log -Append -Encoding ASCII
Start-Sleep -Seconds 5
(Get-Date -Format o) + ' Z: present = ' + (Test-Path Z:\) | Out-File $log -Append -Encoding ASCII