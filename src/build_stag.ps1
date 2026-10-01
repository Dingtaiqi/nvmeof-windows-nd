$ErrorActionPreference = "Continue"
# Build the NVMe-oF STag smoke test against the same NDSPI stack rdmaio uses.
$vs    = if ($env:ND_VS_DIR) { $env:ND_VS_DIR } else { "F:\Microsoft Visual Studio\18\Community" }
$vsdev = "$vs\Common7\Tools\VsDevCmd.bat"
$ndInc = if ($env:ND_NDUTIL_INC) { $env:ND_NDUTIL_INC } else { "D:\rdma\NetworkDirect\src\ndutil" }
$mxlInc = if ($env:ND_MLNX_INC) { $env:ND_MLNX_INC } else { "C:\Program Files\Mellanox\MLNX_VPI\IB\SDK\inc\ndv2" }
$ndLib = if ($env:ND_NDUTIL_LIB) { $env:ND_NDUTIL_LIB } else { "D:\rdma\NetworkDirect\src\x64\Release" }
$src = $PSScriptRoot            # 脚本自己所在目录：clone 到哪都能跑
if (-not (Test-Path "$vsdev")) { Write-Host "VsDevCmd not found: $vsdev"; exit 1 }
foreach ($p in @($ndInc, $ndLib, $src)) {
    if (-not (Test-Path $p)) { Write-Host "missing: $p"; exit 1 }
}
if (-not (Test-Path $mxlInc)) { Write-Host "WARN: Mellanox ndv2 include dir not found: $mxlInc" }

$inc = "/I`"$ndInc`""
if (Test-Path $mxlInc) { $inc += " /I`"$mxlInc`"" }

$cmd = "call `"$vsdev`" -arch=x64 -no_logo >nul 2>&1 && cd /d `"$src`" && " +
       "cl /nologo /W4 /std:c++17 /EHsc $inc stag_smoketest.cpp " +
       "/Fe:stag_smoketest.exe /Fo:stag_smoketest.obj " +
       "/link /LIBPATH:`"$ndLib`" ndutil.lib ws2_32.lib"

Write-Host "building stag_smoketest.exe ..."
& cmd.exe /c $cmd 2>&1 | ForEach-Object { "  $_" }
Write-Host "exit=$LASTEXITCODE"
if (Test-Path "$src\stag_smoketest.exe") {
    Get-Item "$src\stag_smoketest.exe" | Select-Object Name, Length, LastWriteTime | Format-List
}
