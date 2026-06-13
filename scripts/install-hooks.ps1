# install-hooks.ps1 — install the git pre-commit hook that runs the verification
# harness (build + tests) and blocks the commit on failure. Run once after
# `git init`. Idempotent.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"

$hooksDir = Join-Path $RepoRoot '.git\hooks'
if (-not (Test-Path $hooksDir)) {
    throw "no .git/hooks directory; run 'git init' first"
}

$hook = Join-Path $hooksDir 'pre-commit'
$lf = "`n"
$content =
    "#!/bin/sh$lf" +
    "# Auto-installed by scripts/install-hooks.ps1.$lf" +
    "# Runs build + tests; a nonzero exit blocks the commit (Iron Rule 1).$lf" +
    "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"scripts/precommit.ps1`"$lf" +
    "exit `$?$lf"

# Write LF-only, no BOM, so Git-for-Windows' sh can run it.
$enc = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($hook, $content, $enc)

Write-Ok "installed pre-commit hook -> $hook"
exit 0
