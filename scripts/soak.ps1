# soak.ps1 — long-haul walk-bot soak harness. STUB until M10 (the real soak:
# 8 h headless walk, telemetry percentiles, periodic connectivity audits,
# contactsheet, minidump capture). For now it builds and smoke-runs the app so
# the command exists and is wired.
[CmdletBinding()]
param([int]$Hours = 8)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"

Write-Note "soak.ps1 is a stub until M10 (requested hours: $Hours)."
Invoke-CMakeBuild

$app = Join-Path (Get-BinDir) 'backrooms.exe'
if (Test-Path $app) {
    & $app --version | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "app exited nonzero during smoke run" }
    Write-Ok "app smoke run clean; full soak harness lands in M10"
} else {
    Write-Note "app not built yet; nothing to soak"
}
exit 0
