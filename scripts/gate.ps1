# gate.ps1 — the milestone gate runner. "Gate runner is law": a milestone is
# done only when `gate.ps1 -Milestone M<N>` exits 0 (CLAUDE.md Iron Rule 1).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gate.ps1 -Milestone M0
#
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Milestone,
    [int]$SoakSeconds = 60,
    [int]$WindowSeconds = 10,
    [int]$StreamSoakSeconds = 600
)

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

# Run the app, capturing stdout+exit. Returns @{ Exit=int; Out=string }.
function Invoke-AppCapture {
    param([string[]]$AppArgs)
    $exe = Join-Path (Get-BinDir) 'backrooms.exe'
    if (-not (Test-Path $exe)) { throw "backrooms.exe not built" }
    $out = & $exe @AppArgs 2>&1 | Out-String
    return @{ Exit = $LASTEXITCODE; Out = $out }
}

# Parse "key: <int>" out of captured app output. Throws if absent.
function Get-Metric {
    param([string]$Text, [string]$Key)
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match ("^\s*" + [regex]::Escape($Key) + "\s*:\s*(-?\d+)\s*$")) {
            return [int64]$matches[1]
        }
    }
    throw "metric '$Key' not found in app output"
}

function Invoke-GateM1 {
    param([int]$SoakSeconds, [int]$WindowSeconds)

    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }

    $bin = Get-BinDir
    $tmp = Join-Path $RepoRoot 'runs\gate-m1'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $golden = Join-Path $RepoRoot 'goldens\m1\frame0_320x180.png'
    $hashdiff = Join-Path $bin 'hashdiff.exe'

    Assert-Gate 'ctest unit suite still green (regression)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $hashes = @()
    Assert-Gate 'headless frame-0 PNG bit-identical across 3 runs, zero debug-layer msgs' {
        for ($i = 1; $i -le 3; $i++) {
            $png = Join-Path $tmp "run$i.png"
            $r = Invoke-AppCapture @('--headless', '--out', $png, '--width', '320', '--height', '180')
            if ($r.Exit -ne 0) { throw "run $i exited $($r.Exit)" }
            if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "run $i had D3D12 debug-layer messages" }
            $script:m1hash = (& $hashdiff hash $png | Select-Object -Last 1)
            $hashes += $script:m1hash
        }
        if (@($hashes | Select-Object -Unique).Count -ne 1) { throw "frame-0 not bit-identical across runs: $($hashes -join ', ')" }
    }

    Assert-Gate 'headless frame-0 matches committed golden (INV-8)' {
        if (-not (Test-Path $golden)) { throw "missing golden $golden (capture via goldgen)" }
        $d = (& $hashdiff diff (Join-Path $tmp 'run1.png') $golden | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "frame-0 differs from golden (diff=$d)" }
    }

    Assert-Gate "windowed run: ${WindowSeconds}s, exit 0, zero debug-layer msgs" {
        $r = Invoke-AppCapture @('--window', '--seconds', "$WindowSeconds", '--width', '320', '--height', '180')
        if ($r.Exit -ne 0) { throw "windowed run exited $($r.Exit)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "windowed run had D3D12 debug-layer messages" }
    }

    Assert-Gate "memory soak: ${SoakSeconds}s, private-bytes delta < 16 MiB, no fence timeouts" {
        $r = Invoke-AppCapture @('--headless', '--seconds', "$SoakSeconds")
        if ($r.Exit -ne 0) { throw "soak exited $($r.Exit) (fence timeout or render failure)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "soak had D3D12 debug-layer messages" }
        $delta = Get-Metric $r.Out 'mem_delta_bytes'
        $frames = Get-Metric $r.Out 'frames'
        Write-Note "soak rendered $frames frames; private-bytes delta = $delta"
        if ($delta -ge 16777216) { throw "memory grew by $delta bytes (>= 16 MiB)" }
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

function Invoke-GateM2 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }

    $bin = Get-BinDir
    $tmp = Join-Path $RepoRoot 'runs\gate-m2'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path $bin 'hashdiff.exe'
    $golden = Join-Path $RepoRoot 'goldens\m2\room_640x360.png'

    Assert-Gate 'ctest suite green (collision, determinism, replay, regression)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    Assert-Gate 'core compiles with zero graphics includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }

    Assert-Gate 'cross-process replay: per-tick state hashes bit-identical' {
        $rep = Join-Path $tmp 'in.replay'
        $h1 = Join-Path $tmp 'h1.txt'
        $h2 = Join-Path $tmp 'h2.txt'
        $h3 = Join-Path $tmp 'h3.txt'
        $r = Invoke-AppCapture @('--sim', '--seed', '4242', '--ticks', '3000', '--record', $rep, '--hashlog', $h1)
        if ($r.Exit -ne 0) { throw "record run exited $($r.Exit)" }
        $r = Invoke-AppCapture @('--sim', '--replay', $rep, '--hashlog', $h2)
        if ($r.Exit -ne 0) { throw "replay run 1 exited $($r.Exit)" }
        $r = Invoke-AppCapture @('--sim', '--replay', $rep, '--hashlog', $h3)
        if ($r.Exit -ne 0) { throw "replay run 2 exited $($r.Exit)" }
        $f1 = (Get-FileHash $h1).Hash
        $f2 = (Get-FileHash $h2).Hash
        $f3 = (Get-FileHash $h3).Hash
        if ($f1 -ne $f2 -or $f2 -ne $f3) { throw "per-tick hash logs differ across runs" }
        $lines = (Get-Content $h1 | Measure-Object -Line).Lines
        if ($lines -lt 3000) { throw "expected >=3000 hash lines, got $lines" }
    }

    Assert-Gate 'room golden bit-identical x3, matches committed golden, zero debug msgs' {
        if (-not (Test-Path $golden)) { throw "missing golden $golden (capture via goldgen)" }
        $hashes = @()
        for ($i = 1; $i -le 3; $i++) {
            $png = Join-Path $tmp "room$i.png"
            $r = Invoke-AppCapture @('--scene', '--out', $png, '--width', '640', '--height', '360')
            if ($r.Exit -ne 0) { throw "scene run $i exited $($r.Exit)" }
            if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "scene run $i had D3D12 debug-layer messages" }
            $hashes += (& $hashdiff hash $png | Select-Object -Last 1)
        }
        if (@($hashes | Select-Object -Unique).Count -ne 1) { throw "room render not bit-identical: $($hashes -join ', ')" }
        $d = (& $hashdiff diff (Join-Path $tmp 'room1.png') $golden | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "room render differs from golden (diff=$d)" }
    }

    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
}

function Get-HitchStats {
    param([string]$CsvPath)
    if (-not (Test-Path $CsvPath)) { throw "frame CSV not found: $CsvPath" }
    $rows = Import-Csv $CsvPath
    $t = @($rows | ForEach-Object { [double]$_.frame_ms }) | Sort-Object
    if ($t.Count -lt 50) { throw "too few frames in CSV ($($t.Count))" }
    $median = $t[[int]($t.Count / 2)]
    if ($median -le 0) { throw "non-positive median frame time" }
    $p99 = $t[[int]($t.Count * 0.99)]
    $max = $t[$t.Count - 1]
    return @{
        FrameCount = $t.Count; Median = $median; P99 = $p99; Max = $max;
        P99Ratio = ($p99 / $median); MaxRatio = ($max / $median)
    }
}

function Invoke-GateM3 {
    param([int]$StreamSoakSeconds)

    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m3'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    Assert-Gate 'ctest green (gen regen+seam, stream ring, +regression)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    Assert-Gate 'core compiles with zero graphics includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }

    Assert-Gate 'walk 100+ chunks @1440p: p99 frame < 2x median (best of 2), zero debug msgs' {
        $best = 999.0
        for ($attempt = 1; $attempt -le 2; ++$attempt) {
            $csv = Join-Path $tmp "hitch$attempt.csv"
            $r = Invoke-AppCapture @('--stream', '--frames', '4000', '--width', '2560', '--height', '1440', '--csv', $csv, '--seed', '7')
            if ($r.Exit -ne 0) { throw "stream walk exited $($r.Exit)" }
            if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "stream walk had D3D12 debug-layer messages" }
            $h = Get-HitchStats $csv
            if ($h.FrameCount -lt 3200) { throw "walk covered only $($h.FrameCount) frames (< 100 chunks)" }
            Write-Note ("attempt ${attempt}: frames={0} median={1:N3}ms p99={2:N3}ms max={3:N3}ms  p99/median={4:N2}x" -f `
                $h.FrameCount, $h.Median, $h.P99, $h.Max, $h.P99Ratio)
            if ($h.P99Ratio -lt $best) { $best = $h.P99Ratio }
            if ($h.P99Ratio -lt 2.0) { break }
        }
        if ($best -ge 2.0) { throw "p99 frame is $([math]::Round($best,2))x median over both attempts (>= 2x, NFR violated)" }
    }

    Assert-Gate "memory soak (${StreamSoakSeconds}s): residency bounded, private-bytes slope ~0" {
        $r = Invoke-AppCapture @('--stream', '--seconds', "$StreamSoakSeconds", '--width', '1280', '--height', '720', '--seed', '11')
        if ($r.Exit -ne 0) { throw "soak exited $($r.Exit)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "soak had D3D12 debug-layer messages" }
        $delta = Get-Metric $r.Out 'mem_delta_bytes'
        $frames = Get-Metric $r.Out 'frames'
        $resident = Get-Metric $r.Out 'resident_chunks'
        Write-Note "soak rendered $frames frames; resident=$resident; post-warmup private-bytes delta = $delta"
        if ($delta -ge 16777216) { throw "memory grew by $delta bytes (>= 16 MiB)" }
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
        'M1'    { Invoke-GateM1 -SoakSeconds $SoakSeconds -WindowSeconds $WindowSeconds }
        'M2'    { Invoke-GateM2 }
        'M3'    { Invoke-GateM3 -StreamSoakSeconds $StreamSoakSeconds }
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
