# soak.ps1 — M10 walk-bot soak harness (the real long-haul run + the gate's short
# soak). Runs the headless soak with AUTO-RESTART-AND-LOG on a captured crash, then
# analyzes the frame CSV (FPS percentiles, memory slope) and tiles the periodic
# screenshots into a contact sheet with a mechanical all-black/white screen.
#
#   scripts/soak.ps1 -Hours 8                 # the real long-haul acceptance run
#   scripts/soak.ps1 -Seconds 30 -NoBuild     # the M10 gate's short soak
#   scripts/soak.ps1 -CrashDrill -NoBuild     # forced-crash -> minidump + restart
#
# Prints `key: value` metrics parsed by Invoke-GateM10. Exit 0 = healthy.
[CmdletBinding()]
param(
    [int]$Hours = 0,
    [int]$Minutes = 0,
    [int]$Seconds = 0,
    [uint64]$Seed = 7,
    [int]$Width = 1280,
    [int]$Height = 720,
    [int]$ShotEvery = 400,
    [int]$MaxRestarts = 5,
    [double]$FpsFloor = 30.0,
    [double]$MemSpreadMaxMB = 48.0,
    [int]$DirectorInterval = 15,
    [switch]$Director,
    [switch]$CrashDrill,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"

if (-not $NoBuild) { Invoke-CMakeBuild }

$bin = Get-BinDir
$exe = Join-Path $bin 'backrooms.exe'
$sheetTool = Join-Path $bin 'contactsheet.exe'
if (-not (Test-Path $exe)) { throw "backrooms.exe not built" }

# Run a native exe, capturing stdout+stderr as text + the exit code, WITHOUT
# throwing on a nonzero exit or stderr output (the forced-crash drill exits 70 and
# prints to stderr; under ErrorActionPreference=Stop that would otherwise become a
# terminating NativeCommandError).
function Invoke-Exe([string]$file, [string[]]$argList) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $text = & $file @argList 2>&1 | Out-String } finally { $ErrorActionPreference = $prev }
    return @{ Out = $text; Exit = $LASTEXITCODE }
}

$run = Join-Path $RepoRoot 'runs\soak'
$shots = Join-Path $run 'shots'
$crash = Join-Path $run 'crash'
$csv = Join-Path $run 'frames.csv'
$log = Join-Path $run 'soak.log'
if (Test-Path $run) { Remove-Item -Recurse -Force $run }
New-Item -ItemType Directory -Force -Path $shots | Out-Null
New-Item -ItemType Directory -Force -Path $crash | Out-Null

# --- crash drill: inject a fault, prove minidump + auto-restart-and-log ----------
if ($CrashDrill) {
    Write-Step "soak crash drill: forced fault -> minidump -> restart"
    $r = Invoke-Exe $exe @('--crash-test', '--crash-dir', $crash)
    $code = $r.Exit
    $dump = Join-Path $crash 'minidump.dmp'
    $dumpOk = (Test-Path $dump) -and ((Get-Item $dump).Length -gt 0)
    Add-Content $log "crash detected (exit $code); minidump=$dumpOk; restarting"
    # The restart: a brief clean soak must succeed (auto-restart-and-log works).
    $r2 = Invoke-Exe $exe @('--soak', '--seconds', '5', '--seed', "$Seed", '--width', '640', '--height', '360', '--csv', $csv, '--crash-dir', $crash)
    $restartCode = $r2.Exit
    Add-Content $log "restart exit=$restartCode"
    Write-Ok "crash drill complete (exit $code, restart $restartCode)"
    Write-Output "crash_exit: $code"
    Write-Output ("minidump_exists: {0}" -f ([int]$dumpOk))
    Write-Output ("restart_clean: {0}" -f ([int]($restartCode -eq 0)))
    if (($code -eq 70) -and $dumpOk -and ($restartCode -eq 0)) { exit 0 } else { exit 1 }
}

# --- normal soak with auto-restart-and-log --------------------------------------
$total = $Hours * 3600 + $Minutes * 60 + $Seconds
if ($total -le 0) { $total = 60 }
Write-Step "soak: ${total}s walk-bot @ ${Width}x${Height} seed $Seed (auto-restart x$MaxRestarts)"

$restarts = 0
$start = Get-Date
$lastOut = ''
for (;;) {
    $elapsed = ((Get-Date) - $start).TotalSeconds
    $remaining = [int]($total - $elapsed)
    if ($remaining -le 0) { break }
    $soakArgs = @('--soak', '--seconds', "$remaining", '--seed', "$Seed", '--width', "$Width", '--height', "$Height", '--csv', $csv, '--out', $shots, '--shot-every', "$ShotEvery", '--crash-dir', $crash)
    if ($Director) { $soakArgs += @('--director', '--director-interval', "$DirectorInterval") }  # M12 acceptance: Director ON
    $r = Invoke-Exe $exe $soakArgs
    $lastOut = $r.Out
    $code = $r.Exit
    if ($code -eq 0) { break }
    if ($code -eq 70) {
        $restarts++
        Add-Content $log "crash (exit 70) -> restart $restarts (minidump in $crash)"
        Write-Note "captured crash; auto-restart $restarts/$MaxRestarts"
        if ($restarts -ge $MaxRestarts) { throw "exceeded $MaxRestarts restarts" }
        continue
    }
    throw "soak app failed (exit $code): $lastOut"
}

# Parse the soak app summary (last successful run).
function Field([string]$text, [string]$key) {
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match ("^\s*" + [regex]::Escape($key) + "\s*:\s*(-?\d+)\s*$")) { return [int64]$matches[1] }
    }
    return -1
}
$auditFail = Field $lastOut 'audit_failures'
$stuck = Field $lastOut 'stuck_events'
$dbg = Field $lastOut 'debug_error_count'
$dirProduced = Field $lastOut 'director_produced'

# --- analyze the frame CSV: FPS percentiles + memory slope ----------------------
if (-not (Test-Path $csv)) { throw "frame CSV not written: $csv" }
$rows = Import-Csv $csv
if ($rows.Count -lt 200) { throw "too few soak frames ($($rows.Count))" }
$ft = @($rows | ForEach-Object { [double]$_.frame_ms }) | Sort-Object
$median = $ft[[int]($ft.Count / 2)]
$p99 = $ft[[int]($ft.Count * 0.99)]
$fpsMedian = if ($median -gt 0) { 1000.0 / $median } else { 0.0 }
$fps1pctLow = if ($p99 -gt 0) { 1000.0 / $p99 } else { 0.0 }

# Memory: skip the first 30% (ring fill / warm-up), measure the steady-state spread.
$mem = @($rows | ForEach-Object { [double]$_.mem_bytes })
$skip = [int]($mem.Count * 0.30)
$steady = $mem[$skip..($mem.Count - 1)]
$memMin = ($steady | Measure-Object -Minimum).Minimum
$memMax = ($steady | Measure-Object -Maximum).Maximum
$memSpreadMB = ($memMax - $memMin) / 1MB

# --- contact sheet + mechanical screen ------------------------------------------
$black = -1; $white = -1; $tiles = -1
$sheetPng = Join-Path $run 'contactsheet.png'
if (Test-Path $sheetTool) {
    $sheetOut = (Invoke-Exe $sheetTool @($shots, $sheetPng)).Out
    $black = Field $sheetOut 'black_frames'
    $white = Field $sheetOut 'white_frames'
    $tiles = Field $sheetOut 'tiles'
}

# Metrics on the output stream (Write-Output) so a caller can capture them; the
# decorative Write-Step/Write-Ok lines stay on the host stream.
Write-Output ("soak_frames: {0}" -f $rows.Count)
Write-Output ("soak_restarts: {0}" -f $restarts)
Write-Output ("soak_director_produced: {0}" -f $dirProduced)
Write-Output ("soak_audit_failures: {0}" -f $auditFail)
Write-Output ("soak_stuck_events: {0}" -f $stuck)
Write-Output ("soak_debug: {0}" -f $dbg)
Write-Output ("soak_fps_median: {0:N1}" -f $fpsMedian)
Write-Output ("soak_fps_1pct_low: {0:N1}" -f $fps1pctLow)
Write-Output ("soak_mem_spread_mb: {0:N2}" -f $memSpreadMB)
Write-Output ("contactsheet_tiles: {0}" -f $tiles)
Write-Output ("contactsheet_black: {0}" -f $black)
Write-Output ("contactsheet_white: {0}" -f $white)

$healthy = ($auditFail -eq 0) -and ($stuck -eq 0) -and ($dbg -eq 0) `
    -and ($fps1pctLow -ge $FpsFloor) -and ($memSpreadMB -le $MemSpreadMaxMB) `
    -and ($black -eq 0) -and ($white -eq 0)
if ($healthy) { Write-Ok "soak healthy"; exit 0 } else { Write-Fail "soak unhealthy"; exit 1 }
