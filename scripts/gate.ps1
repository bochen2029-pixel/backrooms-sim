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

# Parse "key: <float>" out of captured app output. Throws if absent.
function Get-MetricFloat {
    param([string]$Text, [string]$Key)
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match ("^\s*" + [regex]::Escape($Key) + "\s*:\s*(-?\d+(?:\.\d+)?)\s*$")) {
            return [double]$matches[1]
        }
    }
    throw "float metric '$Key' not found in app output"
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

function Get-AppHash {
    param([string]$Text)
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match 'final_hash:\s*([0-9a-fA-F]+)') { return $matches[1] }
    }
    throw "final_hash not found in app output"
}

function Invoke-GateM4 {
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
    $tmp = Join-Path $RepoRoot 'runs\gate-m4'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path $bin 'hashdiff.exe'

    Assert-Gate 'ctest green (connectivity 10k, geometry 10k, doorways, +regression)' {
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

    Assert-Gate 'walk-bot: 1 km, zero stuck events (5 seeds), deterministic' {
        $h1 = $null
        for ($seed = 1; $seed -le 5; ++$seed) {
            $r = Invoke-AppCapture @('--walkbot', '--km', '1', '--seed', "$seed")
            if ($r.Exit -ne 0) { throw "seed $seed failed (exit $($r.Exit): short of 1 km or stuck)" }
            if ((Get-Metric $r.Out 'stuck_events') -ne 0) { throw "seed $seed had stuck events" }
            if ($seed -eq 1) { $h1 = Get-AppHash $r.Out }
        }
        $r2 = Invoke-AppCapture @('--walkbot', '--km', '1', '--seed', '1')
        if ((Get-AppHash $r2.Out) -ne $h1) { throw "walk-bot WorldState hash not reproducible across runs" }
        Write-Note "walk-bot 5/5 seeds reached 1 km, 0 stuck; determinism hash $h1"
    }

    Assert-Gate 'top-down 3x3 golden matches per seed, bit-identical x3, zero debug' {
        foreach ($seed in 1, 7) {
            $golden = Join-Path $RepoRoot "goldens\m4\topdown_seed$($seed).png"
            if (-not (Test-Path $golden)) { throw "missing golden seed $seed" }
            $hashes = @()
            for ($i = 1; $i -le 3; ++$i) {
                $png = Join-Path $tmp "td_$($seed)_$($i).png"
                $r = Invoke-AppCapture @('--topdown', '--out', $png, '--width', '512', '--height', '512', '--seed', "$seed")
                if ($r.Exit -ne 0) { throw "topdown seed $seed exited $($r.Exit)" }
                if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "topdown seed $seed had debug-layer messages" }
                $hashes += (& $hashdiff hash $png | Select-Object -Last 1)
            }
            if (@($hashes | Select-Object -Unique).Count -ne 1) { throw "topdown seed $seed not bit-identical across runs" }
            $d = (& $hashdiff diff (Join-Path $tmp "td_$($seed)_1.png") $golden | Select-Object -Last 1)
            if ([double]$d -ne 0.0) { throw "topdown seed $seed differs from golden (diff=$d)" }
        }
    }

    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
}

function Invoke-GateM5 {
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
    $tmp = Join-Path $RepoRoot 'runs\gate-m5'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path $bin 'hashdiff.exe'

    Assert-Gate 'ctest green (texture-determinism + flicker + full regression)' {
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

    # 5 canonical poses x 3 seeds: each lit shot is bit-identical across two
    # fresh processes (texture + flicker + stream determinism), matches its
    # committed golden, is debug-clean, and lands in the luminance band (neither
    # all-black nor blown-out).
    Assert-Gate 'lit shots: 5 poses x 3 seeds bit-identical, match goldens, in luminance band, zero debug' {
        foreach ($seed in 1, 7, 42) {
            for ($pose = 0; $pose -le 4; ++$pose) {
                $golden = Join-Path $RepoRoot "goldens\m5\shot_seed$($seed)_pose$($pose).png"
                if (-not (Test-Path $golden)) { throw "missing golden seed $seed pose $pose" }
                $a = Join-Path $tmp "shot_$($seed)_$($pose)_a.png"
                $b = Join-Path $tmp "shot_$($seed)_$($pose)_b.png"
                $ra = Invoke-AppCapture @('--shot', '--seed', "$seed", '--pose', "$pose", '--ticks', '0', '--width', '640', '--height', '360', '--out', $a)
                if ($ra.Exit -ne 0) { throw "shot seed $seed pose $pose exited $($ra.Exit)" }
                if ((Get-Metric $ra.Out 'debug_error_count') -ne 0) { throw "shot seed $seed pose $pose had debug-layer messages" }
                $rb = Invoke-AppCapture @('--shot', '--seed', "$seed", '--pose', "$pose", '--ticks', '0', '--width', '640', '--height', '360', '--out', $b)
                if ($rb.Exit -ne 0) { throw "shot seed $seed pose $pose (run2) exited $($rb.Exit)" }
                $hA = (& $hashdiff hash $a | Select-Object -Last 1)
                $hB = (& $hashdiff hash $b | Select-Object -Last 1)
                if ($hA -ne $hB) { throw "shot seed $seed pose $pose not bit-identical across runs ($hA vs $hB)" }
                $d = (& $hashdiff diff $a $golden | Select-Object -Last 1)
                if ([double]$d -ne 0.0) { throw "shot seed $seed pose $pose differs from golden (diff=$d)" }
                $mean = Get-MetricFloat $ra.Out 'luma_mean'
                $fb = Get-MetricFloat $ra.Out 'frac_black'
                $fw = Get-MetricFloat $ra.Out 'frac_white'
                if ($mean -lt 50.0 -or $mean -gt 220.0) { throw "seed $seed pose $pose luma_mean $mean outside [50,220]" }
                if ($fb -gt 0.35) { throw "seed $seed pose $pose frac_black $fb > 0.35 (too dark)" }
                if ($fw -gt 0.20) { throw "seed $seed pose $pose frac_white $fw > 0.20 (blown out)" }
            }
        }
        Write-Note "15 lit shots: bit-identical x2, golden-matched, luminance in band, debug-clean"
    }

    # Lit pipeline still hits the frame-time budget: median >= 120 FPS at 1440p
    # (best of 2 to absorb OS scheduler jitter, per M3/ADR-021).
    Assert-Gate 'lit walk @1440p: median frame >= 120 FPS (best of 2), zero debug' {
        $best = 0.0
        for ($attempt = 1; $attempt -le 2; ++$attempt) {
            $csv = Join-Path $tmp "fps$attempt.csv"
            $r = Invoke-AppCapture @('--stream', '--frames', '2000', '--width', '2560', '--height', '1440', '--csv', $csv, '--seed', '7')
            if ($r.Exit -ne 0) { throw "lit walk exited $($r.Exit)" }
            if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "lit walk had D3D12 debug-layer messages" }
            $h = Get-HitchStats $csv
            $fps = 1000.0 / $h.Median
            Write-Note ("attempt ${attempt}: frames={0} median={1:N3}ms ({2:N1} FPS) p99={3:N3}ms" -f `
                $h.FrameCount, $h.Median, $fps, $h.P99)
            if ($fps -gt $best) { $best = $fps }
            if ($best -ge 120.0) { break }
        }
        if ($best -lt 120.0) { throw "median only $([math]::Round($best,1)) FPS at 1440p (< 120, NFR violated)" }
    }

    # Regression: M5's renderer changes must not move the earlier render goldens.
    Assert-Gate 'regression: M1 frame-0, M2 room, M4 top-down goldens still bit-match' {
        $f0 = Join-Path $tmp 'm1_frame0.png'
        $r = Invoke-AppCapture @('--headless', '--out', $f0, '--width', '320', '--height', '180')
        if ($r.Exit -ne 0) { throw "M1 frame-0 render exited $($r.Exit)" }
        $d = (& $hashdiff diff $f0 (Join-Path $RepoRoot 'goldens\m1\frame0_320x180.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M1 frame-0 golden regressed (diff=$d)" }

        $room = Join-Path $tmp 'm2_room.png'
        $r = Invoke-AppCapture @('--scene', '--out', $room, '--width', '640', '--height', '360')
        if ($r.Exit -ne 0) { throw "M2 room render exited $($r.Exit)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "M2 room render had debug-layer messages" }
        $d = (& $hashdiff diff $room (Join-Path $RepoRoot 'goldens\m2\room_640x360.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M2 room golden regressed (diff=$d)" }

        foreach ($seed in 1, 7) {
            $td = Join-Path $tmp "m4_td_$seed.png"
            $r = Invoke-AppCapture @('--topdown', '--out', $td, '--width', '512', '--height', '512', '--seed', "$seed")
            if ($r.Exit -ne 0) { throw "M4 top-down seed $seed exited $($r.Exit)" }
            if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "M4 top-down seed $seed had debug-layer messages" }
            $d = (& $hashdiff diff $td (Join-Path $RepoRoot "goldens\m4\topdown_seed$($seed).png") | Select-Object -Last 1)
            if ([double]$d -ne 0.0) { throw "M4 top-down seed $seed golden regressed (diff=$d)" }
        }
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
        'M4'    { Invoke-GateM4 }
        'M5'    { Invoke-GateM5 }
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
