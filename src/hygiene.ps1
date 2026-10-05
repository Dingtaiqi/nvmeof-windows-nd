# SPDX-License-Identifier: Apache-2.0
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

    # The file list comes from git, so a scratch file in a build directory is not
    # judged and a tracked file cannot be skipped by being absent from a hand-written
    # list.
    #
    # --others --exclude-standard TOO, which is not a detail: `git ls-files` alone
    # lists only files git already knows about, so a NEW document or script is skipped
    # by a run made before `git add` - i.e. exactly the run a developer does before
    # committing, and exactly the file a re-encoding mistake is most likely to be in.
    # This was found by testing the check against a deliberately mangled document that
    # it reported as PASS: the document was new, so it was never looked at.  In CI the
    # distinction disappears (everything is tracked after checkout), which is why the
    # bug could only ever appear locally - the place the check exists to help.
    $tracked = @(& git ls-files --cached --others --exclude-standard)
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

    # Every tracked .md, not a hard-coded list.  A new doc is exactly where a
    # re-encoding mistake would go unnoticed, and the rule is about the text, not
    # about the files that happened to exist when this check was written.
    #
    # THREE DETECTORS, because there are three ways this goes wrong and the first
    # version of this check only caught one of them:
    #
    #  1. UTF-8 read as a WESTERN ANSI code page and re-encoded - the text fills up
    #     with "A-tilde" style pairs.  Caught by the two patterns below.
    #  2. UTF-8 read as GBK/CP936 (code page 936, i.e. THIS machine's default) and
    #     re-encoded.  It produces 閹?/闁?/璺嚎鍥? style text and, critically, it
    #     matched NEITHER of the two original patterns: the check reported PASS on a
    #     document whose first 400 characters had been deliberately mangled that way.
    #     A check that cannot fail on the failure you actually have is not a check.
    #  3. A lossy conversion, which leaves literal U+FFFD in the new file.
    #
    # For (2) there is no exact signature: the byte-level round trip does not survive
    # the lossy decode, and the punctuation ratio does not move (measured: 12.7% after
    # mangling against 13.7% before).  What DOES separate is the character histogram -
    # GBK-misreading UTF-8 pushes Chinese into the rare block U+9000-U+9FFF:
    #
    #     DESIGN.md 7.2%   EVIDENCE-1TB.md 6.2%   INTEROP_F5.md 6.5%
    #     README.zh-CN.md 7.0%   ROADMAP.md 9.0%   <-- every legitimate doc
    #     ROADMAP.md with its text GBK-mangled: 26.2%
    #
    # 15% sits in the middle of that gap with room on both sides.  It is a heuristic
    # and is written down as one: a document that is legitimately ABOUT rare characters
    # would trip it, and the message says what to check rather than just "failed".
    $rareThreshold = 15
    foreach ($f in ($tracked | Where-Object { $_ -match '\.md$' })) {
        $p = Join-Path $Root $f
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $text = [System.IO.File]::ReadAllText($p, [System.Text.Encoding]::UTF8)
        if ($text -match '[\u00C2-\u00C3][\u0080-\u00BF]' -or
            $text -match '[\u00E0-\u00EF][\u0080-\u00BF][\u0080-\u00BF]') {
            $bad += "mojibake (UTF-8 read as ANSI and re-encoded): $f"
            continue
        }
        if ($text -match '\uFFFD') {
            $bad += "mojibake (a lossy conversion left U+FFFD in): $f"
            continue
        }
        $cjk  = ([regex]::Matches($text, '[\u4E00-\u9FFF]')).Count
        $rare = ([regex]::Matches($text, '[\u9000-\u9FFF]')).Count
        if ($cjk -ge 200) {
            $pct = 100.0 * $rare / $cjk
            if ($pct -ge $rareThreshold) {
                $bad += ("possible GBK mojibake: $f - {0:N1}% of its CJK characters are in " +
                         "U+9000-U+9FFF (every legitimate doc here is 6-9%); check it was not " +
                         "re-saved after being read as code page 936") -f $pct
            }
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
