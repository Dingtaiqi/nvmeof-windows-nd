@echo off
rem build_d2.cmd - build nvmeofnd2.sys (the D1.2 remote-namespace miniport).
rem Same shape as the D1.1 build that is installed on this machine:
rem   cl /kernel /guard:cf   +   link /DRIVER /GUARD:CF storport.lib
rem plus the WSK import library (netio.lib) that nvmeof_tcpk.c needs, and a
rem renamed DriverEntry for nvmeof_tcpk.c so its stub does not collide with the
rem miniport's own DriverEntry.  ASCII only.
setlocal
call "F:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 (echo VCVARS FAILED & exit /b 1)

set WDKINC=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0
set WDKLIB=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0
set SRC=D:\rdma\nvmeof\src\driver
set OUT=%SRC%\sym\d2
if not exist "%OUT%" mkdir "%OUT%"

set CFLAGS=/nologo /c /kernel /W4 /GS /guard:cf /GF /Gy /Oi /Zl /D_AMD64_ /DAMD64 /D_WIN64 /D_WIN32_WINNT=0x0A00 /D_WINVER=0x0A00 /DNTDDI_VERSION=0x0A00000C /DPOOL_NX_OPTIN=1 /DNDEBUG
set INCS=/I"%WDKINC%\km" /I"%WDKINC%\km\crt" /I"%WDKINC%\shared"

echo === compiling nvmeofnd.c ===
cl %CFLAGS% %INCS% /Fo"%OUT%\nvmeofnd.obj" "%SRC%\nvmeofnd.c"
if errorlevel 1 (echo CL FAILED nvmeofnd.c & exit /b 2)

echo === compiling nvmeof_tcpk.c (its DriverEntry renamed) ===
cl %CFLAGS% %INCS% /DDriverEntry=NvtcpkLibraryEntry /Fo"%OUT%\nvmeof_tcpk.obj" "%SRC%\nvmeof_tcpk.c"
if errorlevel 1 (echo CL FAILED nvmeof_tcpk.c & exit /b 3)

echo === linking ===
link /nologo /DRIVER:WDM /SUBSYSTEM:NATIVE,1.10 /MACHINE:X64 /GUARD:CF /OPT:REF /OPT:ICF /DEBUG /ENTRY:GsDriverEntry /LIBPATH:"%WDKLIB%\km\x64" /OUT:"%OUT%\nvmeofnd2.sys" "%OUT%\nvmeofnd.obj" "%OUT%\nvmeof_tcpk.obj" ntoskrnl.lib hal.lib wdm.lib storport.lib netio.lib BufferOverflowK.lib
if errorlevel 1 (echo LINK FAILED & exit /b 4)

rem Sign here, not by hand later: a freshly linked driver is unsigned, and when one
rem was deployed unsigned the device refused to start with problem 52
rem (CM_PROB_UNSIGNED_DRIVER, 0xC0000428) on 2026-10-18 - the build and the deploy
rem both looked fine and only the device knew.  testsigning is on, so the local test
rem certificate is enough.
echo === signing ===
set SIGNTOOL=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe
"%SIGNTOOL%" sign /fd sha256 /s My /n "NVMeoF Test Driver" "%OUT%\nvmeofnd2.sys"
if errorlevel 1 (echo SIGN FAILED & exit /b 5)

echo === result ===
dir "%OUT%\nvmeofnd2.sys"
exit /b 0
