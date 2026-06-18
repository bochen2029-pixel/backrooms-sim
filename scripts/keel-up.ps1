# keel-up.ps1 -- bring up the SELF-CONTAINED LLM sidecar (llama-server :8080 + keel-serve :7071) from
# the bundled runtime under dist\Backrooms -- with ZERO external C:\ dependency (ADR-076). This is the
# in-tree replacement for the old C:\keel-sidecar-7071\start.cmd: everything the sidecar needs (the CUDA
# llama stack, keel-serve, the GGUF models) lives inside the repo's own dist\ bundle.
#
# Why it exists: the dev exe (build\bin\backrooms.exe) has no co-located runtime\, so ITS
# try_start_sidecar() would fall back to C:\. Pre-starting the BUNDLED sidecar here means the dev
# gates/tests connect to an already-running :7071 (director::service_up reuse) and never touch C:\.
#
# Idempotent: a port already answering is left alone. Stop cleanly with scripts\keel-down.ps1.
# Windows PowerShell 5.1 compatible (no ternary / && / ??).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/keel-up.ps1          # 9B + mmproj (vision)
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/keel-up.ps1 -Use4B   # 4B text-only tier
[CmdletBinding()]
param([switch]$Use4B)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"

$bundle = Join-Path $RepoRoot 'dist\Backrooms'
$rt     = Join-Path $bundle 'runtime'
$models = Join-Path $bundle 'models'
$logs   = Join-Path $bundle 'logs'
if (-not (Test-Path $rt)) {
    Write-Fail "bundled runtime not found at $rt -- stage it first: scripts\package.ps1 (the sidecar is self-contained in dist\, never C:\)"
    exit 1
}
New-Item -ItemType Directory -Force -Path $logs | Out-Null

$llamaExe = Join-Path $rt 'llama\llama-server.exe'
$keelExe  = Join-Path $rt 'keel\keel-serve.exe'
$modelName = 'Qwen3.5-9B-Q5_K_M.gguf'
if ($Use4B) { $modelName = 'Qwen3.5-4B-Q4_K_M.gguf' }
$model  = Join-Path $models $modelName
$mmproj = Join-Path $models 'mmproj-F16.gguf'
foreach ($f in @($llamaExe, $keelExe, $model)) {
    if (-not (Test-Path $f)) { Write-Fail "missing bundled component: $f"; exit 1 }
}

function Test-Port([int]$p) {
    $c = New-Object System.Net.Sockets.TcpClient
    try { $c.Connect('127.0.0.1', $p); return $c.Connected } catch { return $false } finally { $c.Close() }
}
function Wait-Port([int]$p, [int]$timeoutSec, [string]$what) {
    Write-Step "waiting for $what on :$p (up to ${timeoutSec}s)..."
    for ($i = 0; $i -lt $timeoutSec; $i++) {
        if (Test-Port $p) { Write-Ok "$what up on :$p"; return $true }
        Start-Sleep -Seconds 1
    }
    return $false
}

# 1) llama-server :8080 FIRST (keel reuses it). Args mirror app/src/main.cpp try_start_sidecar().
if (Test-Port 8080) {
    Write-Note "llama-server already answering on :8080 -- left alone (idempotent)"
} else {
    $args = "-m `"$model`" --host 127.0.0.1 --port 8080 -ngl 99 -c 8192"
    if (-not $Use4B) { $args = "-m `"$model`" --mmproj `"$mmproj`" --host 127.0.0.1 --port 8080 -ngl 99 -c 8192" }
    Write-Step "launching bundled llama-server ($modelName) -> :8080 (CUDA, loading into VRAM)"
    Start-Process -FilePath $llamaExe -ArgumentList $args -WorkingDirectory (Split-Path $llamaExe) `
        -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $logs 'llama.out.log') `
        -RedirectStandardError  (Join-Path $logs 'llama.err.log') | Out-Null
    if (-not (Wait-Port 8080 180 'llama-server')) { Write-Fail "llama-server did not come up (see $logs\llama.err.log)"; exit 1 }
}

# 2) keel-serve :7071 (cwd = its dir; reads keel.lock; reuses :8080).
if (Test-Port 7071) {
    Write-Note "keel-serve already answering on :7071 -- left alone (idempotent)"
} else {
    Write-Step "launching bundled keel-serve -> :7071"
    Start-Process -FilePath $keelExe -ArgumentList 'keel.lock' -WorkingDirectory (Split-Path $keelExe) `
        -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $logs 'keel.out.log') `
        -RedirectStandardError  (Join-Path $logs 'keel.err.log') | Out-Null
    if (-not (Wait-Port 7071 60 'keel-serve')) { Write-Fail "keel-serve did not come up (see $logs\keel.err.log)"; exit 1 }
}

Write-Ok "self-contained sidecar READY -- :8080 (llama, $modelName) + :7071 (keel), entirely from dist\Backrooms (no external C:\)"
exit 0
