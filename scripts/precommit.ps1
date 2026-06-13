# precommit.ps1 — invoked by the git pre-commit hook (Iron Rule: a deliberately
# failing test must block a commit). Runs quickcheck (build + tests); a nonzero
# exit aborts the commit.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"

Write-Step "pre-commit: build + tests must pass"
& (Join-Path $PSScriptRoot 'quickcheck.ps1')
$rc = $LASTEXITCODE
if ($rc -ne 0) {
    Write-Fail "pre-commit blocked the commit (quickcheck exit $rc)"
    exit 1
}
Write-Ok "pre-commit checks passed"
exit 0
