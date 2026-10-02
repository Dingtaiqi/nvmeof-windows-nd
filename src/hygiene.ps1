# SPDX-License-Identifier: AGPL-3.0-or-later
<#
  Repository hygiene: the three checks that CI runs on every push, in one place so a
  developer can run the SAME code before pushing.

  Why this exists as a script rather than as a block inside ci.yml: the rule was in CI
  only, and it went red on a push whose author had no way to run it locally.  The three
  checks each exist because a mistake of exactly that shape was committed here:

    * a new file without an SPDX header;
    * a .ps1 rewritten without its UTF-8 BOM - Windows PowerShell 5.1 then reads it as
      ANSI, and every non-ASCII string comparison in it breaks;
    * a doc re-encoded from BOM-less UTF-8, so Chinese text became mojibake.

  The BOM rule is not hypothetical and it is easy to trip from outside an editor: any
  tool that writes UTF-8 without a BOM strips it, and the file still runs on PowerShell 7
  and still looks correct in a diff.  It happened again in the commit that added this
  script - four .ps1 files went out without their BOM and CI failed on exactly it.

  Exit code 0 = clean, 1 = at least one failure.  In CI the failures are also emitted as
  ::error:: annotations, because a red X whose reason is not visible is not a gate.
#>
[CmdletBinding()]
param(
    [string] $Root = ''      # repository root; default is this script's parent
)

$ErrorActionPreference = 'Stop'

if (-not $Root) {
    $here = $PSScriptRoot
    if (-not $here) { $here = Split-Path -Parent $MyInvocation.MyCommand.Path }
    $Root = (Resolve-Path (Join-Path $here '..')).Path
}
# ABSOLUTE PATHS THROUGHOUT.  .NET static file calls resolve a relative path against the
# PROCESS working directory, not against PowerShell's current location - so a run from a
# different directory reported every file as "missing a BOM" (a false failure is worse
# than no check: it trains the reader to ignore the output).
$Root = (Resolve-Path $Root).Path
Push-Location $Root
try {
    $bad = @()

    # The file list comes from git, so an untracked scratch file is not judged and a
    # tracked file cannot be skipped by being absent from a hand-written list.
    $tracked = @(& git ls-files)
    if ($LASTEXITCODE -ne 0 -or $tracked.Count -eq 0) {
        Write-Host "hygiene: 'git ls-files' produced nothing; is this a git checkout?"
        exit 1
    }

    foreach ($f in $tracked) {
        $p = Join-Path $Root $f
        if (-not (Test-Path -LiteralPath $p)) { $bad += "tracked but missing: $f"; continue }

        if ($f -match '\.(c|cpp|h|ps1|sh|py)$') {
            $head = (Get-Content -LiteralPath $p -TotalCount 5 -ErrorAction SilentlyContinue) -join "`n"
            if ($head -notmatch 'SPDX-License-Identifier') { $bad += "SPDX missing: $f" }
        }
        if ($f -match '\.ps1$') {
            $bytes = [System.IO.File]::ReadAllBytes($p)
            if ($bytes.Length -lt 3 -or $bytes[0] -ne 0xEF -or $bytes[1] -ne 0xBB -or $bytes[2] -ne 0xBF) {
                $bad += "no UTF-8 BOM: $f"
            }
        }
    }

    foreach ($f in @('README.md', 'README.zh-CN.md', 'DESIGN.md')) {
        $p = Join-Path $Root $f
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $text = [System.IO.File]::ReadAllText($p, [System.Text.Encoding]::UTF8)
        if ($text -match '[\u00C2-\u00C3][\u0080-\u00BF]' -or
            $text -match '[\u00E0-\u00EF][\u0080-\u00BF][\u0080-\u00BF]') {
            $bad += "mojibake (UTF-8 read as ANSI and re-encoded): $f"
        }
    }

    if ($bad.Count -gt 0) {
        $bad | ForEach-Object { Write-Host "  $_" }
        # Annotations so the reason is visible without admin rights on the job log.
        $bad | Select-Object -First 10 | ForEach-Object {
            Write-Host ("::error::" + ($_ -replace '%', '%25'))
        }
        Write-Host "hygiene: $($bad.Count) check(s) failed"
        exit 1
    }
    Write-Host "hygiene: PASS (SPDX headers, .ps1 UTF-8 BOMs, doc encoding)"
    exit 0
}
finally {
    Pop-Location
}
