# keel-down.ps1 -- cleanly stop the self-contained sidecar started by keel-up.ps1.
# Stops keel-serve first, then llama-server, ONCE (never in a loop -- hard-looping a CUDA llama-server
# can TDR the GPU; a single clean Stop-Process is fine). Safe to run when nothing is up (no-op).
# Windows PowerShell 5.1 compatible.
$ErrorActionPreference = 'SilentlyContinue'
. "$PSScriptRoot\lib\common.ps1"

foreach ($name in @('keel-serve', 'llama-server')) {
    $procs = Get-Process -Name $name -ErrorAction SilentlyContinue
    if ($procs) {
        Write-Step "stopping $name (PID $($procs.Id -join ', '))"
        $procs | Stop-Process -Force -ErrorAction SilentlyContinue
        Write-Ok "$name stopped"
    } else {
        Write-Note "$name not running"
    }
}
exit 0
