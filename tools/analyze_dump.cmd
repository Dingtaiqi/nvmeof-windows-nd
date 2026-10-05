@echo off
rem SPDX-FileCopyrightText: 2026 Dingtaiqi
rem SPDX-License-Identifier: Apache-2.0
rem
rem analyze_dump.cmd - analyse a crash dump SAFELY and repeatably.
rem
rem Why a batch file and not a PowerShell one-liner: every time this was attempted
rem from a shell, the quoting around -z <dump> was mangled.  kd then starts without a
rem target, and a debugger that ends up in the wrong mode is not something to run on
rem a working machine.  Hard-coding the argument list here removes that failure mode
rem entirely: -z is always present, so this can only ever READ a file.
rem
rem Guard rails, in order:
rem   1. -z <dump> is mandatory in this script; there is no code path without it.
rem   2. -y points at a LOCAL symbol cache first, so a run never storms the network
rem      after the first time.
rem   3. -logo writes to a file, so no shell redirection is involved at all.
rem   4. Prints the first lines so the caller can see it found the dump, not "usage".
rem
rem Usage:  analyze_dump.cmd [path-to-dmp]      (defaults to the newest minidump)
rem Output: dump_analysis.txt next to this script, and on stdout.

setlocal enabledelayedexpansion
set "KD=C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe"
set "SYM=srv*D:\symbols*https://msdl.microsoft.com/download/symbols"
set "OUT=%~dp0dump_analysis.txt"

if not exist "%KD%" (
    echo [X] kd.exe not found at "%KD%"
    exit /b 2
)

set "DUMP=%~1"
if "%DUMP%"=="" (
    for /f "delims=" %%f in ('dir /b /o-d "C:\Windows\Minidump\*.dmp" 2^>nul') do (
        set "DUMP=C:\Windows\Minidump\%%f"
        goto :found
    )
    echo [X] no minidump found under C:\Windows\Minidump
    exit /b 3
)
:found

if not exist "%DUMP%" (
    echo [X] dump not found: "%DUMP%"
    exit /b 4
)

echo [i] kd      : %KD%
echo [i] dump    : %DUMP%
echo [i] symbols : %SYM%
echo [i] log     : %OUT%
echo.

"%KD%" -z "%DUMP%" -y "%SYM%" -logo "%OUT%" -c ".bugcheck; !analyze -v; kb; lm kv; q"

echo.
echo === first lines of %OUT% ===
for /f "delims=" %%l in ('more +0 "%OUT%" 2^>nul') do (
    echo %%l
    goto :tail
)
:tail
echo === end ===
exit /b 0
