# quickcheck.ps1 — fast incremental build + tests. Wired as the PostToolUse hook
# (.claude/settings.json) and used by the pre-commit hook. Exit 2 on failure so
# the failure text is fed back to Claude (per the settings template contract).
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"

try {
    Invoke-CMakeBuild   # incremental
    Push-Location $RepoRoot
    try {
        Write-Step "ctest (incremental)"
        ctest --test-dir build --output-on-failure
        $rc = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($rc -ne 0) {
        Write-Fail "tests failed (ctest exit $rc)"
        exit 2
    }
    Write-Ok "quickcheck passed"
    exit 0
} catch {
    Write-Fail $_.Exception.Message
    exit 2
}
