# check_inventory.ps1 — Iron Rule 7: the ARCHITECTURE.md module inventory must
# match the directory listing, and every module carries a MODULE.md. Exit 1 on
# mismatch.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\lib\common.ps1"

$expected = @('core', 'gen', 'stream', 'render_d3d12', 'render_dxr',
              'audio', 'telemetry', 'director', 'app', 'tools')

$arch = Get-Content (Join-Path $RepoRoot 'docs\ARCHITECTURE.md') -Raw
$problems = @()

foreach ($m in $expected) {
    $dir = Join-Path $RepoRoot $m
    if (-not (Test-Path $dir -PathType Container)) {
        $problems += "module dir missing: $m/"
        continue
    }
    if (-not (Test-Path (Join-Path $dir 'MODULE.md'))) {
        $problems += "MODULE.md missing: $m/MODULE.md"
    }
    if (-not $arch.Contains('`' + $m + '`')) {
        $problems += "not listed in ARCHITECTURE.md inventory: $m"
    }
}

# Flag stray top-level module-like directories not in the inventory. `build-release`
# + `dist` are M17 packaging build artifacts (gitignored), same category as `build`.
# `_run_state` (autopilot control docs: AUTOSTART/ROADMAP/SETUP) + `.brstate` (the
# supervisor's gitignored DONE/STALLED sentinels) are the self-driving rig, same class
# as `runs`/`scripts` — not sim modules, so they carry no MODULE.md / ARCHITECTURE entry.
$known = $expected + @('contracts', 'docs', 'scripts', 'tests', 'goldens', 'runs',
                       'build', 'build-release', 'dist', 'extern', 'files', '.git', '.claude',
                       '_run_state', '.brstate')
Get-ChildItem $RepoRoot -Directory | ForEach-Object {
    if ($known -notcontains $_.Name) {
        $problems += "unexpected top-level directory (not in inventory): $($_.Name)/"
    }
}

if ($problems.Count -gt 0) {
    Write-Fail "module inventory mismatch (Iron Rule 7):"
    $problems | ForEach-Object { Write-Host "    $_" }
    exit 1
}
Write-Ok "module inventory matches ARCHITECTURE.md"
exit 0
