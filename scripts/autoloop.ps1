# autoloop.ps1 — the respawn supervisor that turns the milestone loop self-driving.
#
# It spawns a FRESH headless `claude -p "<AUTOSTART.md>"` session, lets it execute ROADMAP
# slices to ~90% context and exit, then respawns another — making the DISK (SESSION_LOG +
# ROADMAP + git + the m<N>-green tags) the memory and each session disposable. The gate
# (`scripts/gate.ps1`) is the non-model oracle that keeps unsupervised iteration safe: a
# wrong step cannot tag green, and a regression reverts to the last green tag.
#
# One-time setup + the permission posture: _run_state/AUTOSTART_SETUP.md.
#
#   pwsh -File scripts/autoloop.ps1                      # up to 50 sessions; stop after 2 no-progress
#   pwsh -File scripts/autoloop.ps1 -MaxSessions 200 -StallLimit 3
#   pwsh -File scripts/autoloop.ps1 -DryRun              # verify wiring; spawn nothing
#
# Stops on: .brstate\DONE (complete -> perpetual-polish) · .brstate\STALLED (only operator-gated
# items left) · HEAD unchanged -StallLimit sessions (no progress) · -MaxSessions cap. Full trail:
# runs\autoloop.log (and every action is also in git).

[CmdletBinding()]
param(
    [int]$MaxSessions = 50,
    [int]$StallLimit  = 2,
    [switch]$DryRun,
    [switch]$SkipSidecar,
    [string[]]$ClaudeArgs = @('--dangerously-skip-permissions')
)

$ErrorActionPreference = 'Stop'
$RepoRoot   = Split-Path -Parent $PSScriptRoot
$Autostart  = Join-Path $RepoRoot '_run_state\AUTOSTART.md'
$BrState    = Join-Path $RepoRoot '.brstate'
$LogDir     = Join-Path $RepoRoot 'runs'
$LogFile    = Join-Path $LogDir 'autoloop.log'
$DoneFile   = Join-Path $BrState 'DONE'
$StalledFile= Join-Path $BrState 'STALLED'
$SidecarCmd = 'C:\keel-sidecar-7071\start.cmd'

New-Item -ItemType Directory -Force -Path $BrState | Out-Null
New-Item -ItemType Directory -Force -Path $LogDir  | Out-Null

function Write-Log {
    param([string]$Msg)
    $line = '[' + (Get-Date).ToString('s') + '] ' + $Msg
    Write-Host $line
    Add-Content -Path $LogFile -Value $line
}

function Test-Port {
    param([string]$RHost, [int]$Port)
    $client = New-Object System.Net.Sockets.TcpClient
    $ok = $false
    try { $client.Connect($RHost, $Port); $ok = $client.Connected } catch { $ok = $false } finally { $client.Close() }
    return $ok
}

function Ensure-Sidecar {
    if ($SkipSidecar) { return }
    if (Test-Port -RHost '127.0.0.1' -Port 7071) { Write-Log 'KEEL sidecar :7071 is up.'; return }
    if (-not (Test-Path $SidecarCmd)) {
        Write-Log "WARN: KEEL sidecar not running and $SidecarCmd not found; brain/vision gates will graceful-no-op."
        return
    }
    Write-Log 'KEEL sidecar :7071 down -> launching start.cmd ...'
    Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', $SidecarCmd -WindowStyle Minimized | Out-Null
    for ($w = 0; $w -lt 30; $w++) {
        Start-Sleep -Seconds 1
        if (Test-Port -RHost '127.0.0.1' -Port 7071) { Write-Log 'KEEL sidecar :7071 came up.'; return }
    }
    Write-Log 'WARN: KEEL sidecar did not come up within 30 s; brain/vision gates will graceful-no-op.'
}

function Get-Head { return (git -C $RepoRoot rev-parse HEAD).Trim() }

# --- preflight ---------------------------------------------------------------
Set-Location $RepoRoot
if (-not (Test-Path $Autostart)) { Write-Log "FATAL: $Autostart not found."; exit 2 }
$claude = Get-Command claude -ErrorAction SilentlyContinue
if (-not $claude -and -not $DryRun) { Write-Log "FATAL: 'claude' CLI not on PATH."; exit 2 }

if (Test-Path $DoneFile)    { Write-Log "STOP: .brstate\DONE present (build complete). Delete it to re-run / polish."; exit 0 }
if (Test-Path $StalledFile) { Write-Log "STOP: .brstate\STALLED present. Clear ROADMAP S5 ISSUES + delete it to resume."; exit 0 }

$prompt = Get-Content $Autostart -Raw
Write-Log "=== autoloop start - MaxSessions=$MaxSessions StallLimit=$StallLimit DryRun=$DryRun - repo=$RepoRoot ==="

if ($DryRun) {
    Ensure-Sidecar
    Write-Log "[dry-run] HEAD = $(Get-Head)  describe = $((git -C $RepoRoot describe --tags 2>$null))"
    Write-Log "[dry-run] would spawn:  claude -p <_run_state\AUTOSTART.md>  $($ClaudeArgs -join ' ')"
    Write-Log "[dry-run] AUTOSTART.md first line: $(( $prompt -split "`r?`n" )[0])"
    Write-Log "[dry-run] sentinels watched: $DoneFile ; $StalledFile"
    Write-Log '[dry-run] wiring OK; spawned nothing.'
    exit 0
}

# --- the respawn loop --------------------------------------------------------
$stall = 0
for ($i = 1; $i -le $MaxSessions; $i++) {
    if (Test-Path $DoneFile)    { Write-Log "DONE sentinel -> stopping (session $i not started)."; break }
    if (Test-Path $StalledFile) { Write-Log "STALLED sentinel -> stopping (session $i not started)."; break }

    Ensure-Sidecar
    $before = Get-Head
    Write-Log "--- session $i / $MaxSessions · HEAD before = $before ---"

    $cargs = @('-p', $prompt) + $ClaudeArgs
    & $claude.Source @cargs
    $code = $LASTEXITCODE
    Write-Log "session $i exited (code $code)."

    $after = Get-Head
    if ($after -eq $before) {
        $stall++
        Write-Log "no git progress this session ($stall / $StallLimit). HEAD still $after."
    } else {
        $stall = 0
        Write-Log "progress: $before -> $after  (describe = $((git -C $RepoRoot describe --tags 2>$null)))"
    }
    if ($stall -ge $StallLimit) { Write-Log "stall limit reached ($StallLimit no-progress sessions) -> stopping."; break }
}

Write-Log '=== autoloop exiting ==='
