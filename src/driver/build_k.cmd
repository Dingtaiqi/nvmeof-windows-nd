@echo off
rem build_k.cmd - build nvmeofk_lb.sys (the D3 listener + same-card loopback test driver).
rem Same shape as build_d2.cmd: cl /kernel /guard:cf + link /DRIVER /GUARD:CF.
rem ASCII only.
setlocal
call "F:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 (echo VCVARS FAILED & exit /b 1)

set WDKINC=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0
set WDKLIB=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0
set SRC=D:\rdma\nvmeof\src\driver
if "%1"=="" (set NAME=nvmeofk_lb) else (set NAME=%1)
set OUT=%SRC%\sym\k
if not exist "%OUT%" mkdir "%OUT%"

set CFLAGS=/nologo /c /kernel /W4 /GS /guard:cf /GF /Gy /Zl /D_AMD64_ /DAMD64 /D_WIN64 /D_WIN32_WINNT=0x0A00 /D_WINVER=0x0A00 /DNTDDI_VERSION=0x0A00000C /DPOOL_NX_OPTIN=1 /DNDEBUG
set INCS=/I"%WDKINC%\km" /I"%WDKINC%\km\crt" /I"%WDKINC%\shared"

echo === compiling nvmeofk.c ===
cl %CFLAGS% %INCS% /Fo"%OUT%\nvmeofk.obj" "%SRC%\nvmeofk.c"
if errorlevel 1 (echo CL FAILED nvmeofk.c & exit /b 2)

echo === linking ===
link /nologo /DRIVER:WDM /SUBSYSTEM:NATIVE,1.10 /MACHINE:X64 /GUARD:CF /OPT:REF /OPT:ICF /DEBUG /ENTRY:GsDriverEntry /LIBPATH:"%WDKLIB%\km\x64" /OUT:"%OUT%\%NAME%.sys" "%OUT%\nvmeofk.obj" ntoskrnl.lib hal.lib wdm.lib netio.lib BufferOverflowK.lib
if errorlevel 1 (echo LINK FAILED & exit /b 4)

echo === result ===
dir "%OUT%\%NAME%.sys"
exit /b 0

