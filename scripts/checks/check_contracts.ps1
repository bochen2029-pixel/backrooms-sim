# check_contracts.ps1 — M12 doc audit: every boundary has a contract header, each
# header is documented in contracts/README.md, and there are no stray/undocumented
# contract headers. Exit 1 on any mismatch (CI doc check, Iron Rule 7).
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\lib\common.ps1"

$expected = @('geometry_v1', 'world_view_v1', 'replay_v1', 'chunk_gen_v1',
              'stream_events_v1', 'audio_events_v1', 'telemetry_v1', 'director_v1')

$readme = Get-Content (Join-Path $RepoRoot 'contracts\README.md') -Raw
$problems = @()

foreach ($c in $expected) {
    if (-not (Test-Path (Join-Path $RepoRoot "contracts\$c.h"))) {
        $problems += "contract header missing: contracts/$c.h"
    }
    if (-not $readme.Contains("$c.h")) {
        $problems += "contract not documented in contracts/README.md: $c.h"
    }
}

# No stray / undocumented contract headers.
Get-ChildItem (Join-Path $RepoRoot 'contracts') -Filter '*.h' | ForEach-Object {
    if ($expected -notcontains $_.BaseName) {
        $problems += "undocumented contract header: contracts/$($_.Name)"
    }
}

if ($problems.Count -gt 0) {
    Write-Fail "contract coverage problems (Iron Rule 7):"
    $problems | ForEach-Object { Write-Host "    $_" }
    exit 1
}
Write-Ok "every boundary has a documented contract header ($($expected.Count))"
exit 0
