# audit.ps1 — the fast per-step SELF-AUDIT: every cheap non-LLM oracle in one command.
#
# Why this exists (Externality Principle, solo-enterprise-architect v7): ground truth must come
# from OUTSIDE the model. This script runs the project's externalized oracles so drift is caught
# at every step instead of accumulating until the next milestone gate. Run it as a PRE-audit
# (confirm a clean baseline before a change) and a POST-audit (confirm the change introduced no
# drift). It is FASTER than gate.ps1 (no clean rebuild, no GPU goldens) and DEEPER than the
# post-edit quickcheck hook (adds determinism + structural invariants).
#
# Oracles (all non-LLM, all mechanical/runtime):
#   1. build      — incremental compile, warnings-as-errors  (the code is valid)
#   2. ctest      — every unit test passes                    (mechanical proof)
#   3. determinism— shoggoth record == replay hash            (runtime oracle; the sacred invariant)
#   4. inventory  — module list matches ARCHITECTURE.md       (Iron Rule 7 — structural drift)
#   5. isolation  — sim core has zero render/audio/etc deps   (INV-5 — coupling drift)
# Plus a working-tree cleanliness summary (scope-creep / uncommitted-age awareness).
#
# Exit 0 iff every oracle passes. Prints a one-line PASTE-READY verdict for docs/CHANGE_AUDIT_LOG.md.
# Windows PowerShell 5.1 compatible (no ternary / && / ??), like the other project scripts.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/audit.ps1            # full audit
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/audit.ps1 -Quick     # skip determinism (build+test+structure only)
[CmdletBinding()]
param([switch]$Quick)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"

$fails = @()
$notes = @()

function Audit-Check {
    param([string]$Name, [scriptblock]$Body)
    Write-Step "AUDIT: $Name"
    try {
        & $Body
        Write-Ok $Name
    } catch {
        Write-Fail "$Name -- $($_.Exception.Message)"
        $script:fails += $Name
    }
}

# 1) Build (incremental, /WX) ---------------------------------------------------
Audit-Check 'build (incremental, warnings-as-errors)' {
    Invoke-CMakeBuild
}

# 2) ctest (all unit tests) -----------------------------------------------------
$ctestSummary = 'not run'
Audit-Check 'ctest (all unit tests pass)' {
    $build = Join-Path $RepoRoot 'build'
    $out = ctest --test-dir $build --output-on-failure 2>&1 | Out-String
    $code = $LASTEXITCODE
    $m = [regex]::Match($out, '(\d+)% tests passed,\s*(\d+) tests failed out of (\d+)')
    if ($m.Success) { $script:ctestSummary = "$($m.Groups[3].Value) tests, $($m.Groups[2].Value) failed" }
    if ($code -ne 0) {
        $bad = ($out -split "`n" | Select-String 'Failed|\*\*\*') | Select-Object -First 6
        throw "ctest exit $code -- $($bad -join '; ')"
    }
}

# 3) Determinism: shoggoth record == replay (the sacred runtime oracle) ----------
$determHash = 'skipped'
if (-not $Quick) {
    Audit-Check 'determinism (shoggoth record == replay, model offline)' {
        $exe = Join-Path (Get-BinDir) 'backrooms.exe'
        if (-not (Test-Path $exe)) { throw "exe missing (build failed?): $exe" }
        $slog = Join-Path $RepoRoot 'runs\audit_sacred.log'
        $rec = & $exe --shoggoth-record --director-url http://127.0.0.1:7071 --director-log $slog --seed 3 --ticks 1200 2>&1 | Out-String
        $rep = & $exe --shoggoth-replay --director-log $slog 2>&1 | Out-String
        $recH = [regex]::Match($rec, 'combined_hash:\s*(\S+)').Groups[1].Value
        $repH = [regex]::Match($rep, 'combined_hash:\s*(\S+)').Groups[1].Value
        if (-not $recH) { throw "record produced no combined_hash" }
        if ($recH -ne $repH) { throw "record $recH != replay $repH -- DETERMINISM BROKEN" }
        $script:determHash = $recH
        # Note (not a failure): valid_intents needs the KEEL sidecar at :7071. The hash match is
        # the determinism oracle and is KEEL-independent.
        $vi = [regex]::Match($rec, 'valid_intents:\s*(\d+)').Groups[1].Value
        if ($vi -eq '0') { $script:notes += 'determinism proven but valid_intents=0 (KEEL :7071 down -- LLM path not exercised)' }
    }
}

# 4) Module inventory (Iron Rule 7) ---------------------------------------------
Audit-Check 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
    & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "inventory mismatch" }
}

# 5) Core isolation (INV-5) -----------------------------------------------------
Audit-Check 'sim core isolation (INV-5 grep gate)' {
    & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "core isolation breached" }
}

# 6) Working-tree cleanliness summary (informational scope-creep awareness) ------
$treeFiles = 0
$treeStat = ''
$head = ''
$desc = ''
try {
    $porcelain = & git -C $RepoRoot status --porcelain 2>$null
    if ($porcelain) { $treeFiles = ($porcelain | Measure-Object -Line).Lines }
    $treeStat = (& git -C $RepoRoot diff --shortstat 2>$null | Out-String).Trim()
    $head = (& git -C $RepoRoot log --oneline -1 2>$null | Out-String).Trim()
    $desc = (& git -C $RepoRoot describe --tags --always 2>$null | Out-String).Trim()
} catch { }
if ($treeStat -match '(\d+) insertion') {
    $ins = [int]$matches[1]
    if ($ins -gt 400) { $notes += "uncommitted diff $ins lines > 400 (diff-budget / scope-creep watch)" }
}

# Verdict ----------------------------------------------------------------------
Write-Host ''
Write-Host '======================================================================'
foreach ($n in $notes) { Write-Note $n }
$stamp = (Get-Date).ToString('yyyy-MM-ddTHH:mm')
if ($fails.Count -gt 0) {
    Write-Fail "AUDIT FAILED ($stamp): $($fails -join ', ')"
    Write-Host "AUDIT $stamp FAIL -- failed: $($fails -join ', '); ctest=$ctestSummary; determ=$determHash; tree=$treeFiles files"
    exit 1
}
Write-Ok "AUDIT PASSED ($stamp) -- all non-LLM oracles green"
Write-Host "AUDIT $stamp PASS -- build ok | ctest $ctestSummary | determ $determHash | inventory ok | isolation ok | tree $treeFiles files | $desc"
exit 0
