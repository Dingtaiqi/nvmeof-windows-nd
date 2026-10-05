# SPDX-FileCopyrightText: 2026 Dingtaiqi
# SPDX-License-Identifier: Apache-2.0
# ===========================================================================
#  run_auth.ps1 - DH-HMAC-CHAP, with no NIC anywhere in the picture.
#
#  Builds src/test_auth.cpp and runs it, then hands the SAME exe to
#  run_authselftest.ps1, which recomputes the Diffie-Hellman values with
#  System.Numerics.BigInteger.  CNG and BigInteger share no code, so agreement
#  means the values and their byte order are right.
#
#  Why this suite exists at all: the auth self-test used to be reachable only as
#  `f5_interop -authselftest`, and f5_interop.cpp includes nvmeof_rdma.h, which needs
#  the NetworkDirect SDK that is not part of this repository.  The result was that
#  the crypto could only be checked on a machine that already had the whole stack
#  installed - including the one place that should check it on every push.  The two
#  headers need bcrypt and wincrypt and nothing else, so this builds anywhere MSVC
#  does, which is why CI runs it (.github/workflows/ci.yml).
#
#  /W4 /WX on this file too: a warning in a constant-time compare or a byte-order
#  helper is a security-relevant finding, not a style nit.
# ===========================================================================
$ErrorActionPreference = 'Continue'
$src = $PSScriptRoot
$vs    = if ($env:ND_VS_DIR) { $env:ND_VS_DIR } else { "F:\Microsoft Visual Studio\18\Community" }
$vsdev = "$vs\Common7\Tools\VsDevCmd.bat"
$rc = 0

$exe = "$src\test_auth.exe"
$obj = "$src\test_auth.obj"
Remove-Item $exe, $obj -ErrorAction SilentlyContinue

$cmd = "call `"$vsdev`" -arch=x64 -no_logo >nul 2>&1 && cd /d `"$src`" && " +
       "cl /nologo /W4 /WX /std:c++17 /EHsc /I`"$src`" test_auth.cpp " +
       "/Fe:test_auth.exe /Fo:test_auth.obj /link bcrypt.lib crypt32.lib"
Write-Host "building test_auth.exe ..."
$out = & cmd.exe /c $cmd 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "BUILD FAILED (rc=$LASTEXITCODE)" -ForegroundColor Red
    $out | Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" }
    exit 1
}

Write-Host "running the self-test ..."
$run = & $exe 2>&1
$run | ForEach-Object { Write-Host "  $_" }
$failures = $LASTEXITCODE
if ($failures -ne 0) {
    Write-Host "RESULT: FAIL - $failures self-test failure(s)" -ForegroundColor Red
    $rc = 1
}

# The independent cross-check, against a different implementation.
Write-Host "cross-checking the DH values with BigInteger ..."
& (Join-Path $src 'run_authselftest.ps1') -Exe $exe 2>&1 | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) {
    Write-Host "RESULT: FAIL - the BigInteger cross-check disagreed" -ForegroundColor Red
    $rc = 1
}

Remove-Item $obj -ErrorAction SilentlyContinue
if ($rc -eq 0) {
    Write-Host "RESULT: PASS - DH-HMAC-CHAP primitives, protocol pieces and the DH cross-check all agree" -ForegroundColor Green
} else {
    # Leave the exe behind on failure so the log can be reproduced by hand.
    exit $rc
}
Remove-Item $exe -ErrorAction SilentlyContinue
exit $rc
