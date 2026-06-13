# rehydrate.ps1 - prints a concise state brief for context re-hydration.
# Wired as a SessionStart hook (startup|resume|compact) so that immediately after
# any context compaction/summarization the agent is re-grounded in where the
# build stands. The repo is the source of truth; this surfaces the critical bits.
# Always exits 0 (a hook failure must not disrupt session start). ASCII only.
$ErrorActionPreference = 'SilentlyContinue'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

Write-Output "=== BACKROOMS SIM : RE-HYDRATION BRIEF (auto-injected on session start/resume/compact) ==="
Write-Output "Project C:\backrooms - native Win32 C++20 D3D12+DXR Backrooms walking sim, built milestone-by-milestone."
Write-Output "Rules: a milestone is done only when scripts/gate.ps1 -Milestone M<N> exits 0; tag m<N>-green + push on green;"
Write-Output "  revert to last green tag on regression; scripts/*.ps1 run under Windows PowerShell 5.1 (no ternary/??/&&);"
Write-Output "  keep Catch2 TEST_CASE names ASCII. Canon docs/ARCHITECTURE.md, plan docs/MILESTONES.md, report PROGRESS.md."
Write-Output ""

Push-Location $root
Write-Output "--- git ---"
$tag = (git describe --tags 2>$null)
Write-Output ("last tag/describe: " + $tag)
Write-Output "recent commits:"
git log --oneline -5 2>$null | ForEach-Object { Write-Output ("  " + $_) }
$status = (git status --short 2>$null)
if ([string]::IsNullOrWhiteSpace([string]$status)) {
    Write-Output "working tree: clean"
} else {
    Write-Output "working tree has UNCOMMITTED CHANGES:"
    $status | ForEach-Object { Write-Output ("  " + $_) }
}
Pop-Location
Write-Output ""

$logPath = Join-Path $root 'docs\SESSION_LOG.md'
if (Test-Path $logPath) {
    $lines = @(Get-Content $logPath -Encoding UTF8)
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^##\s+Session') { $start = $i; break }
    }
    if ($start -ge 0) {
        $end = $lines.Count
        for ($j = $start + 1; $j -lt $lines.Count; $j++) {
            if ($lines[$j] -match '^##\s+Session') { $end = $j; break }
        }
        Write-Output "--- newest SESSION_LOG entry (source of truth for current state) ---"
        for ($k = $start; $k -lt $end; $k++) { Write-Output $lines[$k] }
    }
}
Write-Output "=== END BRIEF : re-read docs/SESSION_LOG.md + PROGRESS.md before continuing ==="
exit 0
