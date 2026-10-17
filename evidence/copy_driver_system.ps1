# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# copy_driver_system.ps1 - the deploy half of the D1.2 test loop.
#
# The INF's ServiceBinary is %12%\nvmeofnd2.sys, so the image Windows loads is
# C:\Windows\System32\drivers\nvmeofnd2.sys and NOT the one build_d2.cmd writes.
# A normal shell cannot write there, so this runs as SYSTEM from a one-shot
# scheduled task.  The loaded image is locked against replacement, so a failed
# copy falls back to renaming the old image out of the way and copying again;
# both paths record sizes and SHA-256 so a silent failure cannot look like a
# deploy.

$ErrorActionPreference = 'Stop'
$src = 'D:\rdma\nvmeof\src\driver\sym\d2\nvmeofnd2.sys'
$dst = 'C:\Windows\System32\drivers\nvmeofnd2.sys'
$res = 'D:\rdma\nvmeof\evidence\deploy_result.txt'

function W([string]$s)
{
    Add-Content -Path $res -Value $s -Encoding ASCII
}

Remove-Item $res -ErrorAction SilentlyContinue
try
{
    W ('started=' + (Get-Date -Format 'yyyy-MM-ddTHH:mm:ss'))
    W ('srcBytes=' + (Get-Item $src).Length)
    if (Test-Path $dst)
    {
        $b = Get-Item $dst
        W ('dstBytesBefore=' + $b.Length)
        W ('dstTimeBefore=' + $b.LastWriteTime.ToString('yyyy-MM-ddTHH:mm:ss'))
    }
    else
    {
        W 'dstBefore=missing'
    }

    $copied = $false
    try
    {
        Copy-Item $src $dst -Force
        W 'copy=ok'
        $copied = $true
    }
    catch
    {
        W ('copy=FAILED ' + $_.Exception.Message)
    }

    if (-not $copied)
    {
        $old = $dst + '.old-' + (Get-Date -Format 'yyyyMMddHHmmss')
        try
        {
            Move-Item $dst $old -Force
            W ('rename=ok ' + $old)
        }
        catch
        {
            W ('rename=FAILED ' + $_.Exception.Message)
        }
        try
        {
            Copy-Item $src $dst -Force
            W 'copy2=ok'
            $copied = $true
        }
        catch
        {
            W ('copy2=FAILED ' + $_.Exception.Message)
        }
    }

    if ($copied)
    {
        $s = Get-Item $src
        $d = Get-Item $dst
        $h1 = (Get-FileHash $src -Algorithm SHA256).Hash
        $h2 = (Get-FileHash $dst -Algorithm SHA256).Hash
        W ('srcSha256=' + $h1)
        W ('dstSha256=' + $h2)
        W ('dstBytesAfter=' + $d.Length)
        W ('dstTimeAfter=' + $d.LastWriteTime.ToString('yyyy-MM-ddTHH:mm:ss'))
        W ('match=' + (($s.Length -eq $d.Length) -and ($h1 -eq $h2)))
    }
    else
    {
        W 'match=False'
    }
}
catch
{
    W ('FATAL ' + $_.Exception.Message)
}
