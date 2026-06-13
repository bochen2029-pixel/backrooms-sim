# gate.ps1 — the milestone gate runner. "Gate runner is law": a milestone is
# done only when `gate.ps1 -Milestone M<N>` exits 0 (CLAUDE.md Iron Rule 1).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gate.ps1 -Milestone M0
#
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Milestone)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"

$script:Failures = New-Object System.Collections.ArrayList

# Run a soft gate check; failures are collected, not fatal, so the full report
# is produced in one pass.
function Assert-Gate {
    param([string]$Name, [scriptblock]$Body)
    Write-Step "GATE: $Name"
    try {
        & $Body
        Write-Ok $Name
    } catch {
        Write-Fail "$Name -- $($_.Exception.Message)"
        [void]$script:Failures.Add($Name)
    }
}

function Test-HashdiffRoundTrip {
    param([string]$Bin)
    $hashdiff = Join-Path $Bin 'hashdiff.exe'
    $goldgen  = Join-Path $Bin 'goldgen.exe'
    if (-not (Test-Path $hashdiff)) { throw "hashdiff.exe not built" }
    if (-not (Test-Path $goldgen))  { throw "goldgen.exe not built" }

    $tmp = Join-Path $RepoRoot 'runs\gate-m0'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $A = Join-Path $tmp 'a.png'
    $B = Join-Path $tmp 'b.png'
    $C = Join-Path $tmp 'c.png'

    Invoke-Native $goldgen @('synth', $A, '--w', '96', '--h', '96', '--seed', '1234')
    Invoke-Native $goldgen @('synth', $B, '--w', '96', '--h', '96', '--seed', '1234')
    Invoke-Native $goldgen @('synth', $C, '--w', '96', '--h', '96', '--seed', '1234', '--perturb', '24')

    $dAB = (& $hashdiff diff $A $B | Select-Object -Last 1)
    if ($LASTEXITCODE -ne 0) { throw "hashdiff diff A B errored" }
    if ([double]$dAB -ne 0.0) { throw "identical images reported nonzero diff: $dAB" }

    $dAC = (& $hashdiff diff $A $C | Select-Object -Last 1)
    if ($LASTEXITCODE -ne 0) { throw "hashdiff diff A C errored" }
    if ([double]$dAC -le 0.0) { throw "perturbed image reported zero diff: $dAC" }

    $hA = (& $hashdiff hash $A | Select-Object -Last 1)
    $hB = (& $hashdiff hash $B | Select-Object -Last 1)
    $hC = (& $hashdiff hash $C | Select-Object -Last 1)
    if ($hA -ne $hB) { throw "identical images hashed differently ($hA vs $hB)" }
    if ($hA -eq $hC) { throw "perturbed image hashed identically ($hA)" }

    & $hashdiff equal $A $B | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "equal A B should exit 0" }
    & $hashdiff equal $A $C | Out-Null
    if ($LASTEXITCODE -ne 1) { throw "equal A C should exit 1" }
}

function Invoke-GateM0 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'

    # Hard prerequisite: clean build = fresh-clone-equivalent. /WX means any
    # warning is already a build failure ("zero warnings-as-errors violations").
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') {
            throw "warning text found in $log"
        }
    }

    $bin = Get-BinDir

    Assert-Gate 'ctest discovers and passes the unit suite (incl. seed test)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    Assert-Gate 'hashdiff round-trips a known image pair (0 / nonzero)' {
        Test-HashdiffRoundTrip -Bin $bin
    }

    Assert-Gate 'deliberately failing test is nonzero (commit would block)' {
        $canary = Join-Path $bin 'gate_canary.exe'
        if (-not (Test-Path $canary)) { throw "gate_canary.exe not built" }
        & $canary 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            throw "canary unexpectedly passed; gate cannot detect failing tests"
        }
    }

    Assert-Gate 'pre-commit hook installed and wired to the gate' {
        $hook = Join-Path $RepoRoot '.git\hooks\pre-commit'
        if (-not (Test-Path $hook)) {
            throw "no .git/hooks/pre-commit (run scripts/install-hooks.ps1)"
        }
        if (-not ((Get-Content $hook -Raw) -match 'precommit')) {
            throw "pre-commit hook does not call the precommit script"
        }
    }

    Assert-Gate 'core isolation grep gate (INV-5)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }

    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
}

# --- dispatch ---------------------------------------------------------------
Write-Host ""
Write-Step "Running gate for milestone: $Milestone"
$normalized = $Milestone.ToUpper()
try {
    switch ($normalized) {
        'M0'    { Invoke-GateM0 }
        default {
            Write-Fail "no gate defined for milestone '$Milestone'"
            exit 2
        }
    }
} catch {
    Write-Fail "gate aborted: $($_.Exception.Message)"
    exit 1
}

Write-Host ""
if ($script:Failures.Count -eq 0) {
    Write-Ok "GATE $normalized PASSED"
    exit 0
} else {
    Write-Fail "GATE $normalized FAILED ($($script:Failures.Count) check(s)):"
    $script:Failures | ForEach-Object { Write-Host "    - $_" -ForegroundColor Red }
    exit 1
}
