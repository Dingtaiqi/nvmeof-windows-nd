# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
#
# analyze_dump_offline.ps1 - turn a crash dump into a readable diagnosis, OFFLINE.
#
# Why this is safe, after a history of being cautious about debuggers on this machine:
# cdb.exe -z <file> only parses a file on disk.  It does not attach to the running
# kernel and cannot freeze it.  The two full-system freezes earlier in this project
# happened when kd.exe was started WITHOUT -z: with no /debug boot option it went off
# to set up local kernel debugging against the live kernel.  -z is the difference, and
# this script always passes it.
#
# Reads MEMORY.DMP (kernel dump, has memory contents) or the newest minidump.
# Uses our own build's PDB so the stack shows function names instead of bare offsets.

param(
    [string]$Dump,
    [string]$SymDir   = 'D:\rdma\nvmeof\src\driver\sym',
    [string]$OutFile  = 'D:\rdma\crash_analysis.txt',
    [switch]$WithMicrosoftSymbols,     # off by default: the symbol server is slow from here
    [int]   $TimeoutSec = 900
)

$ErrorActionPreference = 'Continue'
$Cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
if (-not (Test-Path $Cdb)) { Write-Output ('[X] cdb.exe not found at ' + $Cdb); exit 1 }

# --- pick a dump -----------------------------------------------------------------
if (-not $Dump) {
    if (Test-Path 'C:\Windows\MEMORY.DMP') {
        $Dump = 'C:\Windows\MEMORY.DMP'
    } else {
        $newest = Get-ChildItem 'C:\Windows\Minidump' -Filter '*.dmp' -ErrorAction SilentlyContinue |
                  Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($newest) { $Dump = $newest.FullName }
    }
}
if (-not $Dump -or -not (Test-Path $Dump)) {
    Write-Output '[X] no dump found (looked for C:\Windows\MEMORY.DMP and C:\Windows\Minidump\*.dmp)'
    Write-Output '    After the next bugcheck, re-run this script.'
    exit 2
}
$di = Get-Item $Dump
Write-Output ('dump      : ' + $di.FullName)
Write-Output ('size      : ' + [math]::Round($di.Length/1MB,1) + ' MB')
Write-Output ('written   : ' + $di.LastWriteTime)

# --- what our driver's symbols look like -----------------------------------------
Write-Output 'symbols   :'
Get-ChildItem $SymDir -Filter '*.pdb' -ErrorAction SilentlyContinue | ForEach-Object { Write-Output ('            ' + $_.Name + '  ' + [math]::Round($_.Length/1KB,0) + ' KB  ' + $_.LastWriteTime) }
if (-not (Get-ChildItem $SymDir -Filter 'nvmeofk*.pdb' -ErrorAction SilentlyContinue)) {
    Write-Output '            [w] no nvmeofk*.pdb in the symbol directory - the stack will show bare addresses'
}

$symPath = $SymDir
if ($WithMicrosoftSymbols) {
    $symPath = 'srv*C:\symbols*https://msdl.microsoft.com/download/symbols;' + $SymDir
    Write-Output '            (+ the Microsoft symbol server; this can be slow from this network)'
} else {
    $symPath = $SymDir + ';C:\symbols'
}

# --- the questions to ask --------------------------------------------------------
# The globals are the whole point: a stale or wild NDK object pointer in one of them is
# the most likely cause of an access violation at DISPATCH_LEVEL during bring-up.
$cmds = @(
    '.echo ==== BUGCHECK ====',
    '.bugcheck',
    '.echo ==== ANALYZE ====',
    '!analyze -v',
    '.echo ==== REGISTERS AT FAULT ====',
    'r',
    '.echo ==== STACK ====',
    'kb',
    '.echo ==== OUR MODULE ====',
    'lm vm nvmeofk5',
    'lm vm nvmeofk4',
    'lm vm nvmeofk3',
    '.echo ==== THREAD ====',
    '!thread',
    '.echo ==== OUR GLOBALS (NDK objects) ====',
    'dq nvmeofk5!g_async L8',
    'dq nvmeofk5!g_shared L8',
    'dq nvmeofk5!g_qp L4',
    'dq nvmeofk5!g_pd L4',
    'dq nvmeofk5!g_cq L4',
    'dq nvmeofk5!g_mr L4',
    'dq nvmeofk5!g_connector L4',
    'dq nvmeofk5!g_region L4',
    'dq nvmeofk5!g_mdl L2',
    '.echo ==== OUR TRACE LOG (registry copy is in the log text, but the pool block may hold more) ====',
    '!pool nvmeofk5',
    '.echo ==== DONE ====',
    'q'
) -join '; '

Write-Output ''
Write-Output 'running cdb.exe -z ... (offline file parsing only; the live kernel is not touched)'
Write-Output ('output    : ' + $OutFile)
Write-Output ''

# Write the commands to a file and hand cdb that file with -cf.  Passing them with -c
# means one argument containing spaces and semicolons, and Start-Process does not quote
# arguments for you - the string gets split, cdb receives garbage and sits at its prompt
# forever.  A command file has no quoting problem at all.
$cmdFile = [System.IO.Path]::ChangeExtension($OutFile, '.cmds.txt')
Set-Content -Path $cmdFile -Value ($cmds -split '; ') -Encoding ASCII
if (Test-Path $OutFile) { Remove-Item $OutFile -Force -ErrorAction SilentlyContinue }
$p = Start-Process -FilePath $Cdb -ArgumentList @('-z', $Dump, '-y', $symPath, '-cf', $cmdFile, '-logo', $OutFile) -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    Write-Output ('[w] cdb did not finish within ' + $TimeoutSec + 's - stopping it; the log so far is still useful')
    $p.Kill()
}

if (Test-Path $OutFile) {
    Write-Output ('log: ' + [math]::Round((Get-Item $OutFile).Length/1KB,0) + ' KB')
    Write-Output ''
    Write-Output '================ the interesting parts ================'
    $txt = Get-Content $OutFile -ErrorAction SilentlyContinue
    # surface the lines that matter instead of making the reader scroll 2000 lines
    $txt | Select-String -Pattern 'BUGCHECK_CODE|BUGCHECK_P|MODULE_NAME|IMAGE_NAME|FAILURE_BUCKET_ID|FAULTING_IP|PROCESS_NAME|STACK_TEXT|nvmeofk|IRQL|EXCEPTION_CODE|READ_ADDRESS|WRITE_ADDRESS|Unable to load image|Symbol search path' |
        Select-Object -First 80 | ForEach-Object { Write-Output ('  ' + $_.Line) }
    Write-Output '======================================================='
} else {
    Write-Output '[X] cdb produced no log'
}
