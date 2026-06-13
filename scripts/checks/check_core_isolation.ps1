# check_core_isolation.ps1 — INV-5: the sim core has zero includes from render,
# audio, director, stream, gen, or OS UI. Mechanical grep gate (CLAUDE.md Iron
# Rule 4 / ARCHITECTURE.md §3.1). Exit 1 on any violation.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\lib\common.ps1"

$coreDir = Join-Path $RepoRoot 'core'
# Forbidden include fragments (module include-paths and OS/UI/graphics headers).
$forbidden = @(
    '"gen/', '"stream/', '"render_d3d12/', '"render_dxr/', '"audio/', '"director/', '"telemetry/',
    '<d3d12', '<dxgi', '<d3d11', '<windows.h', '<Windows.h', 'llama'
)

$violations = @()
$files = Get-ChildItem $coreDir -Recurse -File -Include *.h, *.hpp, *.cpp -ErrorAction SilentlyContinue
foreach ($f in $files) {
    $lineNo = 0
    foreach ($line in (Get-Content $f.FullName)) {
        $lineNo++
        if ($line -match '^\s*#\s*include') {
            foreach ($frag in $forbidden) {
                if ($line.Contains($frag)) {
                    $violations += "$($f.FullName):$lineNo  $($line.Trim())"
                }
            }
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Fail "core isolation violations (INV-5):"
    $violations | ForEach-Object { Write-Host "    $_" }
    exit 1
}
Write-Ok "core isolation clean (INV-5)"
exit 0
