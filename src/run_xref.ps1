# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# Cross-check our NVMe wire constants against the reference header.
#
# This is the cheap one: no adapter, no processes, no timing.  It exists because
# four separate bugs in this project were all "a constant written from memory that
# our own target and our own test then agreed on", and a person reading a header
# carefully is exactly what stops happening after the first few times.
#
# It runs first in the suite because when it fails, every other result is suspect:
# a wire constant that disagrees with the reference means the rest of the suite is
# checking our code against our own misunderstanding.
$ErrorActionPreference = "Continue"
$src = $PSScriptRoot            # 脚本自己所在目录：clone 到哪都能跑
$py  = Get-Command py -ErrorAction SilentlyContinue
if (-not $py) { $py = Get-Command python -ErrorAction SilentlyContinue }
if (-not $py) { Write-Host "no python launcher found"; exit 2 }

Write-Host "===== xref_constants"
$out = & $py.Source -3 "$src\xref_constants.py" 2>&1
$rc = $LASTEXITCODE
$out | ForEach-Object { "  $_" }
exit $rc
