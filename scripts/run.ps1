# run.ps1 — one command, fresh clone to a running exe. Default: build + a headless
# smoke render (device + streaming + procedural materials + lit render readback ->
# PNG) that proves the engine works end to end and exits 0 (CI/gate-friendly).
#
#   scripts/run.ps1                 # build + smoke render -> runs/run-smoke.png
#   scripts/run.ps1 -Window         # launch the windowed app
#   scripts/run.ps1 -Director       # smoke render with the Director probe (KEEL sidecar)
#
[CmdletBinding()]
param(
    [uint64]$Seed = 1,
    [int]$Width = 1280,
    [int]$Height = 720,
    [switch]$Window,
    [switch]$Director,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"

if (-not $NoBuild) { Invoke-CMakeBuild }
$exe = Join-Path (Get-BinDir) 'backrooms.exe'
if (-not (Test-Path $exe)) { throw "backrooms.exe not built" }

if ($Window) {
    Write-Ok "launching the windowed app (close the window to exit)"
    & $exe --window --width $Width --height $Height --seed $Seed
    exit $LASTEXITCODE
}

$runs = Join-Path $RepoRoot 'runs'
New-Item -ItemType Directory -Force -Path $runs | Out-Null
$png = Join-Path $runs 'run-smoke.png'
& $exe --shot --seed $Seed --pose 0 --ticks 0 --width $Width --height $Height --out $png
if ($LASTEXITCODE -ne 0) { Write-Fail "smoke render failed (exit $LASTEXITCODE)"; exit 1 }
Write-Ok "running exe verified -> $png"

if ($Director) {
    Write-Step "Director probe (needs the KEEL sidecar at :7071)"
    & $exe --director-probe --seed $Seed
}
Write-Note "interactive: scripts/run.ps1 -Window   |   path-traced: backrooms --dxr-pt   |   soak: scripts/soak.ps1 -Hours 8 -Director"
exit 0
