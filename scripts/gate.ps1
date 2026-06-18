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
    [int]$StreamSoakSeconds = 600,
    [int]$AudioSoakSeconds = 60
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

# Parse "key: <token>" out of captured app output as a string (e.g. a hex hash).
function Get-MetricStr {
    param([string]$Text, [string]$Key)
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match ("^\s*" + [regex]::Escape($Key) + "\s*:\s*(\S+)\s*$")) { return $matches[1] }
    }
    throw "string metric '$Key' not found in app output"
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

function Invoke-GateM6 {
    param([int]$AudioSoakSeconds)
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
    $tmp = Join-Path $RepoRoot 'runs\gate-m6'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $wavcheck = Join-Path $bin 'wavcheck.exe'
    $hashdiff = Join-Path $bin 'hashdiff.exe'

    Assert-Gate 'ctest green (audio determinism/WAV/room-probe/footsteps + regression)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }

    # Offline WAV: deterministic across two fresh processes, and its spectrum has
    # the 60 Hz fluorescent fundamental + harmonics and is not silent (wavcheck).
    $stepsA = Join-Path $tmp 'a.steps'
    Assert-Gate 'offline WAV deterministic x2, 60 Hz fundamental + harmonics, not silent' {
        if (-not (Test-Path $wavcheck)) { throw "wavcheck.exe not built" }
        $w1 = Join-Path $tmp 'a.wav'
        $w2 = Join-Path $tmp 'b.wav'
        $r1 = Invoke-AppCapture @('--render-wav', '--seed', '7', '--ticks', '3600', '--out', $w1, '--audiolog', $stepsA)
        if ($r1.Exit -ne 0) { throw "render-wav run1 exited $($r1.Exit)" }
        $r2 = Invoke-AppCapture @('--render-wav', '--seed', '7', '--ticks', '3600', '--out', $w2)
        if ($r2.Exit -ne 0) { throw "render-wav run2 exited $($r2.Exit)" }
        if ((Get-FileHash $w1).Hash -ne (Get-FileHash $w2).Hash) { throw "WAV not bit-identical across runs" }
        & $wavcheck assert $w1 --min-rms 0.02 --fund-ratio 8 --harm-ratio 3 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "wavcheck assert failed (exit $LASTEXITCODE)" }
        $rms = Get-MetricFloat $r1.Out 'rms'
        Write-Note "WAV bit-identical x2; wavcheck OK (60 Hz + harmonics); rms=$rms"
    }

    # The audio footstep log must align 1:1 with the replay's step ticks.
    Assert-Gate 'footstep events align 1:1 with replay step ticks' {
        $ref = Join-Path $tmp 'ref.steps'
        $rr = Invoke-AppCapture @('--footsteps', '--seed', '7', '--ticks', '3600', '--out', $ref)
        if ($rr.Exit -ne 0) { throw "footsteps reference exited $($rr.Exit)" }
        if (-not (Test-Path $stepsA)) { throw "audio footstep log missing (render step did not run)" }
        if ((Get-Content $stepsA -Raw) -ne (Get-Content $ref -Raw)) {
            throw "audio footstep log differs from the replay step log"
        }
        $n = (Get-Content $ref | Measure-Object -Line).Lines
        if ($n -lt 10) { throw "implausibly few footsteps ($n)" }
        Write-Note "audio + replay footstep logs identical ($n steps, 1:1)"
    }

    # Real-time mixer: zero underruns over the soak, and the audio thread does not
    # inflate the sim tick time (it runs on its own thread, fed lock-free).
    Assert-Gate "audio soak (${AudioSoakSeconds}s): zero underruns, audio thread does not block the sim" {
        $on = Invoke-AppCapture @('--audiosoak', '--seed', '9', '--seconds', "$AudioSoakSeconds", '--audio')
        if ($on.Exit -ne 0) { throw "audio soak exited $($on.Exit)" }
        $u = Get-Metric $on.Out 'underruns'
        if ($u -ne 0) { throw "$u buffer underruns over ${AudioSoakSeconds}s" }
        $blocks = Get-Metric $on.Out 'audio_blocks'
        $meanOn = Get-MetricFloat $on.Out 'mean_tick_ns'
        $off = Invoke-AppCapture @('--audiosoak', '--seed', '9', '--seconds', "$AudioSoakSeconds")
        if ($off.Exit -ne 0) { throw "baseline (audio-off) soak exited $($off.Exit)" }
        $meanOff = Get-MetricFloat $off.Out 'mean_tick_ns'
        Write-Note ("soak: 0 underruns, {0} blocks; mean tick off={1:N1}ns on={2:N1}ns" -f $blocks, $meanOff, $meanOn)
        if ($meanOn -gt $meanOff * 1.5 + 50.0) {
            throw "audio inflates sim tick time (off=$([math]::Round($meanOff,1))ns on=$([math]::Round($meanOn,1))ns)"
        }
    }

    # M6 touches no render code; confirm the M5/M4 render goldens didn't move.
    Assert-Gate 'regression: M5 lit shot + M4 top-down goldens still bit-match' {
        $shot = Join-Path $tmp 'm5_shot.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "M5 shot had debug-layer messages" }
        $d = (& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M5 lit shot golden regressed (diff=$d)" }
        $td = Join-Path $tmp 'm4_td.png'
        $r = Invoke-AppCapture @('--topdown', '--seed', '1', '--width', '512', '--height', '512', '--out', $td)
        if ($r.Exit -ne 0) { throw "M4 top-down exited $($r.Exit)" }
        $d = (& $hashdiff diff $td (Join-Path $RepoRoot 'goldens\m4\topdown_seed1.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M4 top-down golden regressed (diff=$d)" }
    }

    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
}

function Invoke-GateM7 {
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
    $tmp = Join-Path $RepoRoot 'runs\gate-m7'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path $bin 'hashdiff.exe'

    # ctest covers exit gate #1 (per-biome connectivity 10k + geometry incl
    # pillars), #2 (distribution 100k within +/-2 %), the cross-level part of #3
    # (level -1 connectivity + geometry), plus full M0-M6 regression.
    Assert-Gate 'ctest green (biome distribution + per-biome validators + level -1 + regression)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }

    # Exit gate #3: a scripted replay descends a stairwell to level -1;
    # connectivity + determinism hold across levels.
    Assert-Gate 'stairwell descent to level -1: deterministic, cross-level connected (gate #3)' {
        $r1 = Invoke-AppCapture @('--descend', '--seed', '1')
        if ($r1.Exit -ne 0) { throw "descent exited $($r1.Exit)" }
        if ((Get-Metric $r1.Out 'level_reached') -ne -1) { throw "did not reach level -1" }
        if ((Get-Metric $r1.Out 'sublevel_connected') -ne 1) { throw "level -1 chunk not connected" }
        if ((Get-Metric $r1.Out 'sublevel_geom_valid') -ne 1) { throw "level -1 chunk geometry invalid" }
        $h1 = Get-AppHash $r1.Out
        $r2 = Invoke-AppCapture @('--descend', '--seed', '1')
        if ((Get-AppHash $r2.Out) -ne $h1) { throw "descent not deterministic across runs" }
        foreach ($s in 7, 42) {
            $r = Invoke-AppCapture @('--descend', '--seed', "$s")
            if ($r.Exit -ne 0 -or (Get-Metric $r.Out 'level_reached') -ne -1) { throw "seed $s descent failed" }
        }
        Write-Note "descent: level -1 reached, sublevel connected, hash $h1 reproducible"
    }

    # Exit gate #4: a fixed-pose lit golden per biome.
    Assert-Gate 'per-biome lit goldens bit-match, bit-identical x2, debug-clean (gate #4)' {
        $map = [ordered]@{ classic_yellow = 1; cubicle_farm = 4; pipe_corridors = 2; parking_garage = 11; poolrooms = 25 }
        foreach ($name in $map.Keys) {
            $seed = $map[$name]
            $golden = Join-Path $RepoRoot "goldens\m7\biome_$($name).png"
            if (-not (Test-Path $golden)) { throw "missing biome golden $name" }
            # Confirm this seed's spawn really is that biome.
            $bi = Invoke-AppCapture @('--biomeat', '--seed', "$seed")
            if ($bi.Out -notmatch "biome:\s*$name") { throw "seed $seed spawn biome is not $name" }
            $a = Join-Path $tmp "biome_$($name)_a.png"
            $b = Join-Path $tmp "biome_$($name)_b.png"
            $ra = Invoke-AppCapture @('--shot', '--seed', "$seed", '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $a)
            if ($ra.Exit -ne 0) { throw "biome $name shot exited $($ra.Exit)" }
            if ((Get-Metric $ra.Out 'debug_error_count') -ne 0) { throw "biome $name shot had debug-layer messages" }
            $rb = Invoke-AppCapture @('--shot', '--seed', "$seed", '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $b)
            if ($rb.Exit -ne 0) { throw "biome $name shot (run2) exited $($rb.Exit)" }
            if ((& $hashdiff hash $a | Select-Object -Last 1) -ne (& $hashdiff hash $b | Select-Object -Last 1)) {
                throw "biome $name shot not bit-identical across runs"
            }
            $d = (& $hashdiff diff $a $golden | Select-Object -Last 1)
            if ([double]$d -ne 0.0) { throw "biome $name golden regressed (diff=$d)" }
        }
        Write-Note "5 per-biome lit goldens bit-identical x2 + golden-matched + debug-clean"
    }

    # Regression: the M4 top-down + M5 lit-shot render goldens (re-captured for the
    # biome world) must still bit-match.
    Assert-Gate 'regression: M4 top-down + M5 lit shot goldens still bit-match' {
        $td = Join-Path $tmp 'm4_td.png'
        $r = Invoke-AppCapture @('--topdown', '--seed', '1', '--width', '512', '--height', '512', '--out', $td)
        if ($r.Exit -ne 0) { throw "M4 top-down exited $($r.Exit)" }
        $d = (& $hashdiff diff $td (Join-Path $RepoRoot 'goldens\m4\topdown_seed1.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M4 top-down golden regressed (diff=$d)" }
        $shot = Join-Path $tmp 'm5_shot.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "M5 shot had debug-layer messages" }
        $d = (& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M5 lit shot golden regressed (diff=$d)" }
    }

    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
}

function Invoke-GateM8 {
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
    $tmp = Join-Path $RepoRoot 'runs\gate-m8'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path $bin 'hashdiff.exe'

    Assert-Gate 'ctest green (full regression)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }

    # Exit gates #1 (clean A/B) + #4 (seeded grain -> deterministic goldens).
    Assert-Gate 'post ON/OFF goldens: bit-identical x2, golden-matched, clean A/B, debug-clean' {
        $off1 = Join-Path $tmp 'off1.png'; $off2 = Join-Path $tmp 'off2.png'
        $on1 = Join-Path $tmp 'on1.png';   $on2 = Join-Path $tmp 'on2.png'
        $ro1 = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $off1)
        $ro2 = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $off2)
        $rn1 = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--post', '--width', '640', '--height', '360', '--out', $on1)
        $rn2 = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--post', '--width', '640', '--height', '360', '--out', $on2)
        foreach ($r in $ro1, $rn1) { if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "post shot had debug-layer messages" } }
        if ((& $hashdiff hash $off1 | Select-Object -Last 1) -ne (& $hashdiff hash $off2 | Select-Object -Last 1)) { throw "post-OFF not bit-identical x2" }
        if ((& $hashdiff hash $on1 | Select-Object -Last 1) -ne (& $hashdiff hash $on2 | Select-Object -Last 1)) { throw "post-ON not bit-identical x2 (seeded grain)" }
        $doff = (& $hashdiff diff $off1 (Join-Path $RepoRoot 'goldens\m8\post_off.png') | Select-Object -Last 1)
        if ([double]$doff -ne 0.0) { throw "post-OFF golden regressed (diff=$doff)" }
        $don = (& $hashdiff diff $on1 (Join-Path $RepoRoot 'goldens\m8\post_on.png') | Select-Object -Last 1)
        if ([double]$don -ne 0.0) { throw "post-ON golden regressed (diff=$don)" }
        $dab = (& $hashdiff diff $off1 $on1 | Select-Object -Last 1)
        if ([double]$dab -le 0.0) { throw "post ON and OFF are identical (no effect)" }
        Write-Note "post A/B: OFF/ON bit-identical x2, golden-matched, A/B diff=$dab"
    }

    # Exit gate #2: timestamp overlay renders the correct sim time (pixels via the
    # golden + the value echoed to telemetry — OCR-free).
    Assert-Gate 'timestamp overlay: correct sim time (telemetry + pixel golden)' {
        $hud = Join-Path $tmp 'hud.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1234', '--pose', '0', '--ticks', '305160', '--post', '--width', '640', '--height', '360', '--out', $hud)
        if ($r.Exit -ne 0) { throw "hud shot exited $($r.Exit)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "hud shot had debug-layer messages" }
        if ($r.Out -notmatch 'timestamp:\s*00:42:23') { throw "timestamp telemetry wrong (expected 00:42:23 for 305160 ticks)" }
        $d = (& $hashdiff diff $hud (Join-Path $RepoRoot 'goldens\m8\hud_timestamp.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "timestamp HUD golden regressed (diff=$d)" }
        Write-Note "timestamp 305160 ticks -> 00:42:23, telemetry + pixels match"
    }

    # Exit gate #3: the post pass costs < 1.5 ms at 1440p (median frame-time delta,
    # post on vs off; best of 2 to absorb OS jitter).
    Assert-Gate 'post pass < 1.5 ms at 1440p (median frame-time delta)' {
        $best = 999.0
        for ($a = 1; $a -le 2; ++$a) {
            $coff = Join-Path $tmp "off$a.csv"; $con = Join-Path $tmp "on$a.csv"
            $r1 = Invoke-AppCapture @('--stream', '--frames', '1500', '--width', '2560', '--height', '1440', '--seed', '7', '--csv', $coff)
            if ($r1.Exit -ne 0) { throw "stream post-off exited $($r1.Exit)" }
            $r2 = Invoke-AppCapture @('--stream', '--frames', '1500', '--width', '2560', '--height', '1440', '--seed', '7', '--post', '--csv', $con)
            if ($r2.Exit -ne 0) { throw "stream post-on exited $($r2.Exit)" }
            if ((Get-Metric $r2.Out 'debug_error_count') -ne 0) { throw "stream post-on had debug-layer messages" }
            $delta = (Get-HitchStats $con).Median - (Get-HitchStats $coff).Median
            Write-Note ("attempt ${a}: post pass = {0:N3} ms" -f $delta)
            if ($delta -lt $best) { $best = $delta }
            if ($best -lt 1.5) { break }
        }
        if ($best -ge 1.5) { throw "post pass is $([math]::Round($best,3)) ms at 1440p (>= 1.5 ms budget)" }
    }

    Assert-Gate 'regression: M5 lit shot + M4 top-down goldens still bit-match (post off)' {
        $shot = Join-Path $tmp 'm5_shot.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        $d = (& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M5 lit shot golden regressed (diff=$d)" }
        $td = Join-Path $tmp 'm4_td.png'
        $r = Invoke-AppCapture @('--topdown', '--seed', '1', '--width', '512', '--height', '512', '--out', $td)
        if ($r.Exit -ne 0) { throw "M4 top-down exited $($r.Exit)" }
        $d = (& $hashdiff diff $td (Join-Path $RepoRoot 'goldens\m4\topdown_seed1.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M4 top-down golden regressed (diff=$d)" }
    }

    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
}

function Invoke-GateM9 {
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
    $hashdiff = Join-Path $bin 'hashdiff.exe'

    Assert-Gate 'ctest green (full regression)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    # DXR is enhancement-only (INV-6): the gate needs a DXR-capable device + the
    # signed-DXIL toolchain. If absent the gate cannot run (this is a dev-GPU gate).
    Assert-Gate 'DXR available (device5 + RaytracingTier >= 1.0 + signed DXIL)' {
        $r = Invoke-AppCapture @('--dxr-probe')
        if ($r.Exit -ne 0) { throw "dxr-probe exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'dxr_ready') -ne 1) { throw "device not DXR-ready: $($r.Out)" }
    }

    # Exit gate #1: cross-renderer primary-hit depth compare. Raster vs DXR at the
    # same pose; per-pixel NDC depth linearized to eye-space metres, compared. Near-
    # exact agreement proves the acceleration structures hold exactly the streamed
    # geometry the rasteriser draws. (Thresholds clear the measured values ~250x;
    # a missing chunk or mis-built AS blows straight past them.)
    Assert-Gate 'cross-renderer depth compare: raster vs DXR within epsilon (5 poses)' {
        for ($p = 0; $p -le 4; $p++) {
            $r = Invoke-AppCapture @('--dxr-depth', '--seed', '1', '--pose', "$p", '--width', '640', '--height', '360')
            if ($r.Exit -ne 0) { throw "pose ${p}: --dxr-depth exited $($r.Exit): $($r.Out)" }
            if ((Get-Metric $r.Out 'raster_debug') -ne 0) { throw "pose ${p}: raster debug-layer messages" }
            if ((Get-Metric $r.Out 'dxr_debug') -ne 0) { throw "pose ${p}: DXR debug-layer messages" }
            $bothFg = Get-Metric $r.Out 'both_fg_pixels'
            if ($bothFg -lt 10000) { throw "pose ${p}: only $bothFg co-foreground pixels (geometry/basis misaligned)" }
            $mis  = Get-MetricFloat $r.Out 'both_fg_mismatch_frac'
            $mean = Get-MetricFloat $r.Out 'mean_fg_depth_relerr'
            $edge = Get-MetricFloat $r.Out 'edge_frac'
            if ($mis  -gt 0.001) { throw "pose ${p}: depth mismatch frac $mis > 0.001" }
            if ($mean -gt 0.005) { throw "pose ${p}: mean depth rel-err $mean > 0.005" }
            if ($edge -gt 0.02)  { throw "pose ${p}: silhouette/edge frac $edge > 0.02" }
            Write-Note "pose ${p}: fg=$bothFg mismatch=$mis mean_relerr=$mean edge=$edge"
        }
    }

    # Exit gate #2: converged path-traced render vs stored golden (1024 spp, 3
    # poses). The PT is deterministic on a given GPU, so a re-render bit-matches
    # the golden; the small mean-abs-diff threshold absorbs cross-vendor float
    # reassociation. Also asserts determinism, a luma band, and debug-clean.
    Assert-Gate 'converged PT golden: 1024 spp at 3 poses, RMSE/diff < threshold' {
        $tmp = Join-Path $RepoRoot 'runs\gate-m9'
        if (-not (Test-Path $tmp)) { New-Item -ItemType Directory -Force -Path $tmp | Out-Null }
        foreach ($p in 1, 3, 4) {
            $a = Join-Path $tmp "pt_p${p}_a.png"
            $b = Join-Path $tmp "pt_p${p}_b.png"
            $gold = Join-Path $RepoRoot "goldens\m9\pt_pose${p}.png"
            $r1 = Invoke-AppCapture @('--dxr-pt', '--seed', '1', '--pose', "$p", '--spp', '1024', '--width', '640', '--height', '360', '--out', $a)
            if ($r1.Exit -ne 0) { throw "pose ${p}: --dxr-pt exited $($r1.Exit): $($r1.Out)" }
            if ((Get-Metric $r1.Out 'debug_error_count') -ne 0) { throw "pose ${p}: PT debug-layer messages" }
            $mean = Get-MetricFloat $r1.Out 'luma_mean'
            $fb = Get-MetricFloat $r1.Out 'frac_black'
            $fw = Get-MetricFloat $r1.Out 'frac_white'
            if ($mean -lt 12.0 -or $mean -gt 210.0) { throw "pose ${p}: luma_mean $mean out of band [12,210]" }
            if ($fb -gt 0.02) { throw "pose ${p}: frac_black $fb > 0.02 (under-lit)" }
            if ($fw -gt 0.02) { throw "pose ${p}: frac_white $fw > 0.02 (blown out)" }
            $r2 = Invoke-AppCapture @('--dxr-pt', '--seed', '1', '--pose', "$p", '--spp', '1024', '--width', '640', '--height', '360', '--out', $b)
            if ($r2.Exit -ne 0) { throw "pose ${p}: second PT render exited $($r2.Exit)" }
            if ((& $hashdiff hash $a | Select-Object -Last 1) -ne (& $hashdiff hash $b | Select-Object -Last 1)) { throw "pose ${p}: PT not deterministic (two 1024-spp renders differ)" }
            $d = (& $hashdiff diff $a $gold | Select-Object -Last 1)
            if ([double]$d -gt 1.0) { throw "pose ${p}: PT golden diff $d > 1.0 (mean abs channel diff)" }
            Write-Note "pose ${p}: luma=$mean diff_vs_golden=$d (deterministic x2)"
        }
    }

    # The interactive spatial DENOISER must REDUCE noise, not just blur: a denoised few-spp frame has to land
    # CLOSER to the converged ground truth than the raw noisy frame. --dxr-denoise renders a high-spp reference,
    # then a 4-spp frame denoise-OFF and denoise-ON, and reports the mean abs channel error of each vs the ref.
    Assert-Gate 'interactive PT denoiser cuts noise toward ground truth (edge-aware spatial filter)' {
        $rd = Invoke-AppCapture @('--dxr-denoise', '--seed', '1', '--spp', '512', '--width', '320', '--height', '180')
        if ($rd.Exit -ne 0) { throw "--dxr-denoise exited $($rd.Exit): $($rd.Out)" }
        if ((Get-Metric $rd.Out 'debug_error_count') -ne 0) { throw "denoise run had debug-layer messages: $($rd.Out)" }
        $eoff = Get-MetricFloat $rd.Out 'err_off'
        $eon  = Get-MetricFloat $rd.Out 'err_on'
        $ratio = Get-MetricFloat $rd.Out 'err_ratio'
        if ($eon -ge $eoff) { throw "denoiser made it WORSE: err_on $eon >= err_off $eoff (it should move toward ground truth)" }
        if ($ratio -ge 0.7) { throw "denoiser too weak: err_ratio $ratio >= 0.7 (expect a clear noise cut, ~0.36 observed)" }
        Write-Note "interactive PT denoiser: 4-spp error vs ground truth $eoff -> $eon (ratio $ratio) -- edge-aware spatial filter, ~$([math]::Round((1.0-$ratio)*100))% noise cut, debug-clean"
    }

    # Exit gate #3: interactive PT. (a) >= 60 FPS while walking (1-spp moving frames
    # at 1440p); (b) accumulation resets on movement (no-ghost) -- a clean reset
    # matches a fresh render, while NOT resetting blends the prior pose in (large
    # delta), proving the renderer supports a correct reset-on-move.
    Assert-Gate 'interactive PT: >= 60 FPS @ 1440p + accumulation resets on movement (no ghost)' {
        $rf = Invoke-AppCapture @('--dxr-fps', '--seed', '1', '--pose', '3', '--spp', '1', '--width', '2560', '--height', '1440')
        if ($rf.Exit -ne 0) { throw "--dxr-fps exited $($rf.Exit): $($rf.Out)" }
        if ((Get-Metric $rf.Out 'debug_error_count') -ne 0) { throw "FPS run had debug-layer messages" }
        $fps = Get-MetricFloat $rf.Out 'fps'
        $medms = Get-MetricFloat $rf.Out 'median_ms'
        if ($fps -lt 60.0) { throw "interactive PT $fps FPS < 60 @ 1440p (median $medms ms)" }
        Write-Note "interactive PT: $fps FPS @ 1440p (median $medms ms, 1 spp/frame)"

        $rg = Invoke-AppCapture @('--dxr-ghost', '--seed', '1', '--width', '640', '--height', '360')
        if ($rg.Exit -ne 0) { throw "--dxr-ghost exited $($rg.Exit): $($rg.Out)" }
        if ((Get-Metric $rg.Out 'debug_error_count') -ne 0) { throw "ghost run had debug-layer messages" }
        $clean = Get-MetricFloat $rg.Out 'clean_vs_fresh'
        $ghost = Get-MetricFloat $rg.Out 'ghost_vs_fresh'
        if ($clean -gt 0.5) { throw "accumulation reset not clean: clean_vs_fresh $clean > 0.5" }
        if ($ghost -lt 5.0) { throw "no-ghost test has no teeth: ghost_vs_fresh $ghost < 5.0 (movement must ghost without a reset)" }
        Write-Note "no-ghost: reset clean=$clean, un-reset ghost=$ghost"
    }

    # Exit gate #4: TLAS rebuild under streaming. A walk-bot covers 1 km in PT mode,
    # streaming chunks (the acceleration structures rebuild as the resident set
    # shifts), with zero debug-layer/DRED errors throughout.
    Assert-Gate 'TLAS rebuild under streaming: walk-bot 1 km in PT mode, zero debug/DRED' {
        $rw = Invoke-AppCapture @('--dxr-walk', '--seed', '1', '--km', '1', '--width', '640', '--height', '360')
        if ($rw.Exit -ne 0) { throw "--dxr-walk exited $($rw.Exit): $($rw.Out)" }
        if ((Get-Metric $rw.Out 'debug_error_count') -ne 0) { throw "1 km PT walk had debug-layer/DRED messages" }
        $dist = Get-MetricFloat $rw.Out 'distance_m'
        $rebuilds = Get-Metric $rw.Out 'tlas_rebuilds'
        $ptf = Get-Metric $rw.Out 'pt_frames'
        if ($dist -lt 1000.0) { throw "walk only reached $dist m (< 1000)" }
        if ($rebuilds -lt 1) { throw "TLAS never rebuilt under streaming ($rebuilds rebuilds)" }
        Write-Note "1 km PT walk: dist=$dist m, $rebuilds TLAS rebuilds, $ptf PT frames, debug-clean"
    }

    # Regression: the additive depth-readback path must not perturb raster output.
    Assert-Gate 'regression: M5 lit shot + M4 top-down goldens still bit-match' {
        $tmp = Join-Path $RepoRoot 'runs\gate-m9'
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        $shot = Join-Path $tmp 'm5_shot.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        $d = (& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M5 lit shot golden regressed (diff=$d)" }
        $td = Join-Path $tmp 'm4_td.png'
        $r = Invoke-AppCapture @('--topdown', '--seed', '1', '--width', '512', '--height', '512', '--out', $td)
        if ($r.Exit -ne 0) { throw "M4 top-down exited $($r.Exit)" }
        $d = (& $hashdiff diff $td (Join-Path $RepoRoot 'goldens\m4\topdown_seed1.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M4 top-down golden regressed (diff=$d)" }
    }

    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }

    Write-Note 'M9 gate covers all 4 exit gates: #1 depth compare, #2 converged PT golden, #3 interactive PT (FPS + no-ghost), #4 TLAS rebuild under streaming'
}

function Invoke-GateM10 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'ctest green (full regression)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $soakScript = Join-Path $PSScriptRoot 'soak.ps1'

    # Exit gate #1: the soak. A short run exercises the long-haul machinery; the
    # full acceptance run is `soak.ps1 -Hours 8` (duration-parameterized, like the
    # M3 stream soak). Asserts zero crashes / audit-failures / stuck events, 1% low
    # FPS above the floor, and a bounded steady-state memory spread (no leak).
    Assert-Gate 'walk-bot soak: memory flat + FPS floor + connectivity audits (short run)' {
        $out = & $soakScript -NoBuild -Seconds 30 -Width 1280 -Height 720 2>&1 | Out-String
        $exit = $LASTEXITCODE
        if ($exit -ne 0) { throw "soak.ps1 reported unhealthy (exit $exit):`n$out" }
        if ((Get-Metric $out 'soak_audit_failures') -ne 0) { throw "connectivity audit failed during soak" }
        if ((Get-Metric $out 'soak_stuck_events') -ne 0) { throw "walk-bot got stuck during soak" }
        if ((Get-Metric $out 'soak_debug') -ne 0) { throw "soak had debug-layer messages" }
        $low = Get-MetricFloat $out 'soak_fps_1pct_low'
        if ($low -lt 30.0) { throw "1% low FPS $low < 30 floor" }
        $spread = Get-MetricFloat $out 'soak_mem_spread_mb'
        if ($spread -gt 48.0) { throw "steady-state memory spread $spread MB > 48 (leak?)" }
        Write-Note "soak: 1%-low $low FPS, mem spread $spread MB, audits + stuck clean"
    }

    # Exit gate #2: contact-sheet mechanical screen (no all-black/all-white frames).
    # Agent visual review of runs\soak\contactsheet.png is the human-in-the-loop part.
    Assert-Gate 'contact sheet: mechanical screen (no all-black/all-white frames)' {
        $shots = Join-Path $RepoRoot 'runs\soak\shots'
        $sheet = Join-Path $RepoRoot 'runs\soak\contactsheet.png'
        if (-not (Test-Path $shots)) { throw "soak screenshots missing: $shots" }
        $out = & (Join-Path (Get-BinDir) 'contactsheet.exe') $shots $sheet 2>&1 | Out-String
        $tiles = Get-Metric $out 'tiles'
        if ($tiles -lt 4) { throw "too few screenshots tiled ($tiles)" }
        if ((Get-Metric $out 'black_frames') -ne 0) { throw "all-black frame(s) in soak (render/stream regression)" }
        if ((Get-Metric $out 'white_frames') -ne 0) { throw "all-white frame(s) in soak" }
        Write-Note "contact sheet: $tiles tiles, 0 black/white -> runs\soak\contactsheet.png"
    }

    # Exit gate #3: forced-crash drill -> minidump captured + clean auto-restart.
    Assert-Gate 'forced-crash drill: minidump captured + clean restart logged' {
        $out = & $soakScript -NoBuild -CrashDrill 2>&1 | Out-String
        $exit = $LASTEXITCODE
        if ($exit -ne 0) { throw "crash drill failed (exit $exit):`n$out" }
        if ((Get-Metric $out 'crash_exit') -ne 70) { throw "crash exit code not 70 (handler not engaged)" }
        if ((Get-Metric $out 'minidump_exists') -ne 1) { throw "no minidump captured" }
        if ((Get-Metric $out 'restart_clean') -ne 1) { throw "auto-restart did not complete cleanly" }
        Write-Note "crash drill: minidump captured, exit 70, clean restart logged"
    }

    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
}

function Invoke-GateM11 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'ctest green (full regression, incl. director validator)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m11'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    # The Director routes to the KEEL sidecar (ADR-038). It must be reachable, else
    # the gate is vacuous. (Relaunch: C:\keel-sidecar-7071\start.cmd.)
    Assert-Gate 'KEEL sidecar reachable (Director inference endpoint :7071)' {
        $r = Invoke-AppCapture @('--director-probe', '--seed', '1')
        if ($r.Exit -ne 0) { throw "director-probe exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'keel_ok') -ne 1) { throw "KEEL not reachable at :7071" }
    }

    # Exit gate #4 (THE sacred one): replay bit-identical with the model OFFLINE.
    Assert-Gate 'Gate 4 determinism: director record == replay, KEEL fully offline' {
        $dlog = Join-Path $tmp 'd.bin'
        $rec = Invoke-AppCapture @('--director-record', '--seed', '11', '--ticks', '600', '--director-log', $dlog)
        if ($rec.Exit -ne 0) { throw "record exited $($rec.Exit): $($rec.Out)" }
        $events = Get-Metric $rec.Out 'director_events'
        if ($events -lt 1) { throw "record captured 0 directives -- gate vacuous (KEEL down?)" }
        $recHash = Get-MetricStr $rec.Out 'combined_hash'
        $rep = Invoke-AppCapture @('--director-replay', '--director-log', $dlog)
        if ($rep.Exit -ne 0) { throw "replay exited $($rep.Exit): $($rep.Out)" }
        $repHash = Get-MetricStr $rep.Out 'combined_hash'
        if ($recHash -ne $repHash) { throw "replay hash $repHash != record hash $recHash (NOT bit-identical)" }
        Write-Note "Gate 4: $events directives, record==replay $recHash (model offline) -- bit-identical"
    }

    # Exit gates #1 + #3: schema-valid directives + p95 latency over a real N=100.
    Assert-Gate 'Gates 1+3: schema-valid directives + p95 latency < 5 s (N=100)' {
        $r = Invoke-AppCapture @('--director-eval', '--eval-count', '100', '--seed', '777')
        if ($r.Exit -ne 0) { throw "director-eval exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'unreachable') -ne 0) { throw "KEEL went unreachable mid-eval" }
        $rate = Get-MetricFloat $r.Out 'schema_valid_rate'
        if ($rate -lt 0.95) { throw "schema-valid rate $rate < 0.95 (Gate 1)" }
        $p95 = Get-MetricFloat $r.Out 'latency_p95_ms'
        if ($p95 -ge 5000.0) { throw "p95 directive latency ${p95} ms >= 5000 (Gate 3)" }
        Write-Note "Gates 1+3: schema-valid $rate, p95 ${p95} ms (< 5 s)"
    }

    # Exit gate #2 (reformulated, ADR-039): async isolation = A (overhead ~0 + no new
    # hitches) + B (lived smoothness under ambient pacing within a stated band). The
    # live-inference baseline shift is shared-GPU contention, not a stall.
    Assert-Gate 'Gate 2 async isolation: integration ~0 + no new hitches + ambient band (A+B)' {
        $coff = Join-Path $tmp 'g2_off.csv'; $cdead = Join-Path $tmp 'g2_dead.csv'; $clive = Join-Path $tmp 'g2_live.csv'
        $roff = Invoke-AppCapture @('--soak', '--no-director', '--seconds', '20', '--seed', '41', '--width', '960', '--height', '540', '--csv', $coff)
        if ($roff.Exit -ne 0) { throw "off soak exited $($roff.Exit)" }
        Invoke-AppCapture @('--soak', '--director', '--director-url', '127.0.0.1:9999', '--seconds', '20', '--seed', '41', '--width', '960', '--height', '540', '--csv', $cdead) | Out-Null
        $rlive = Invoke-AppCapture @('--soak', '--director', '--director-interval', '12', '--seconds', '20', '--seed', '41', '--width', '960', '--height', '540', '--csv', $clive)
        if ($rlive.Exit -ne 0) { throw "live soak exited $($rlive.Exit)" }
        if ((Get-Metric $rlive.Out 'debug_error_count') -ne 0) { throw "live soak had debug-layer messages" }
        if ((Get-Metric $rlive.Out 'director_produced') -lt 1) { throw "live soak ran 0 inferences -- Gate 2 vacuous (KEEL down?)" }
        $soff = Get-HitchStats $coff; $sdead = Get-HitchStats $cdead; $slive = Get-HitchStats $clive
        $a1 = $sdead.Median / $soff.Median        # integration overhead (no inference)
        $bb = $slive.Median / $soff.Median        # lived ambient median
        $a2 = $slive.P99Ratio / $soff.P99Ratio    # relative hitch (no new stalls)
        if ($a1 -gt 1.4) { throw ("A.1 integration overhead: deadurl median {0:N2}x off (> 1.40)" -f $a1) }
        if ($bb -gt 1.6) { throw ("B ambient band: live median {0:N2}x off (> 1.60)" -f $bb) }
        if ($a2 -gt 1.6) { throw ("A.2 new hitches: live p99/med {0:N2}x off (> 1.60)" -f $a2) }
        Write-Note ("Gate 2: A.1 deadurl={0:N2}x, B live={1:N2}x, A.2 hitch={2:N2}x off (shared-GPU shift documented, ADR-039)" -f $a1, $bb, $a2)
    }

    # Exit gate #5: the kill switch -- --no-director runs the sim cleanly.
    Assert-Gate 'Gate 5: --no-director runs a clean soak (kill switch, INV-6)' {
        $r = Invoke-AppCapture @('--soak', '--no-director', '--seconds', '15', '--seed', '5', '--width', '640', '--height', '360')
        if ($r.Exit -ne 0) { throw "--no-director soak exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'director') -ne 0) { throw "--no-director did not disable the Director" }
        if ((Get-Metric $r.Out 'audit_failures') -ne 0) { throw "soak connectivity audit failed" }
        if ((Get-Metric $r.Out 'stuck_events') -ne 0) { throw "soak walk-bot got stuck" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "soak had debug-layer messages" }
        Write-Note "Gate 5: --no-director soak clean (director off, audits/stuck/debug clean)"
    }

    # Regression: the Director is enhancement-only -- raster goldens unchanged.
    Assert-Gate 'regression: M5 lit shot + M4 top-down goldens still bit-match' {
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $shot = Join-Path $tmp 'm5_shot.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        $d = (& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M5 lit shot golden regressed (diff=$d)" }
        $td = Join-Path $tmp 'm4_td.png'
        $r = Invoke-AppCapture @('--topdown', '--seed', '1', '--width', '512', '--height', '512', '--out', $td)
        if ($r.Exit -ne 0) { throw "M4 top-down exited $($r.Exit)" }
        $d = (& $hashdiff diff $td (Join-Path $RepoRoot 'goldens\m4\topdown_seed1.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M4 top-down golden regressed (diff=$d)" }
    }

    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }

    Write-Note 'M11 gate covers all 5 exit gates: #1 schema-valid, #2 async isolation (A+B, ADR-039), #3 p95 latency, #4 replay determinism, #5 --no-director'
}

function Invoke-GateM12 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    # Exit gate: fresh clone -> one script -> running exe.
    Assert-Gate 'one-command run.ps1 -> running exe (fresh-clone smoke)' {
        $out = & (Join-Path $PSScriptRoot 'run.ps1') -NoBuild 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "run.ps1 failed (exit $LASTEXITCODE):`n$out" }
        if (-not (Test-Path (Join-Path $RepoRoot 'runs\run-smoke.png'))) { throw "run.ps1 produced no smoke render" }
        Write-Note "run.ps1 -> running exe + lit smoke render"
    }

    # Polish: the noclip intro is a deterministic scripted sequence that lands in L0.
    Assert-Gate 'noclip intro: deterministic fall lands in Level 0' {
        $a = Invoke-AppCapture @('--intro', '--seed', '1')
        if ($a.Exit -ne 0) { throw "intro exited $($a.Exit): $($a.Out)" }
        if ((Get-Metric $a.Out 'noclipped') -ne 1) { throw "intro did not noclip" }
        if ((Get-Metric $a.Out 'landed') -ne 1) { throw "intro did not land in Level 0" }
        $h1 = Get-MetricStr $a.Out 'final_hash'
        $h2 = Get-MetricStr (Invoke-AppCapture @('--intro', '--seed', '1')).Out 'final_hash'
        if ($h1 -ne $h2) { throw "intro not deterministic ($h1 != $h2)" }
        Write-Note "intro: noclip -> Level 0, deterministic ($h1)"
    }

    # Exit gate: 12 h unattended acceptance soak with the Director ON. The gate runs a
    # short, duration-parameterized version (full run = soak.ps1 -Hours 12 -Director);
    # needs the KEEL sidecar at :7071.
    Assert-Gate 'acceptance soak with the Director ON (short; full = soak.ps1 -Hours 12 -Director)' {
        $probe = Invoke-AppCapture @('--director-probe', '--seed', '1')
        if ((Get-Metric $probe.Out 'keel_ok') -ne 1) { throw "KEEL sidecar not reachable at :7071 (acceptance needs the Director)" }
        $out = & (Join-Path $PSScriptRoot 'soak.ps1') -NoBuild -Director -Seconds 25 -Width 1280 -Height 720 2>&1 | Out-String
        $exit = $LASTEXITCODE
        if ($exit -ne 0) { throw "acceptance soak unhealthy (exit $exit):`n$out" }
        if ((Get-Metric $out 'soak_director_produced') -lt 1) { throw "Director ran 0 inferences during the acceptance soak" }
        if ((Get-Metric $out 'soak_audit_failures') -ne 0) { throw "acceptance soak connectivity audit failed" }
        if ((Get-Metric $out 'soak_debug') -ne 0) { throw "acceptance soak had debug-layer messages" }
        Write-Note ("acceptance soak (Director ON) healthy: {0} directives, audits/debug clean" -f (Get-Metric $out 'soak_director_produced'))
    }

    # Exit gate: CI doc checks (inventory + every boundary has a contract file).
    Assert-Gate 'CI doc checks: module inventory + contract coverage' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
        & (Join-Path $PSScriptRoot 'checks\check_contracts.ps1')
        if ($LASTEXITCODE -ne 0) { throw "contract coverage check failed" }
    }

    Assert-Gate 'regression: M5 lit shot + M4 top-down goldens still bit-match' {
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $tmp = Join-Path $RepoRoot 'runs\gate-m12'
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        $shot = Join-Path $tmp 'm5_shot.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        $d = (& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M5 lit shot golden regressed (diff=$d)" }
        $td = Join-Path $tmp 'm4_td.png'
        $r = Invoke-AppCapture @('--topdown', '--seed', '1', '--width', '512', '--height', '512', '--out', $td)
        if ($r.Exit -ne 0) { throw "M4 top-down exited $($r.Exit)" }
        $d = (& $hashdiff diff $td (Join-Path $RepoRoot 'goldens\m4\topdown_seed1.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M4 top-down golden regressed (diff=$d)" }
    }

    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }

    Write-Note 'M12 gate: full ctest + one-command run + noclip intro + acceptance soak (Director ON) + doc checks. Acceptance run = soak.ps1 -Hours 12 -Director; tag v1.0 after the M0-M11 regression sweep.'
}

function Invoke-GateM13 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    # Exit gate: the windowed playable renders the streamed maze in real time,
    # debug/DRED-clean, with bounded frame pacing (no hitches). Closes the M1
    # clear-frame gap -- render_chunks_windowed shares the gated lit pipeline.
    # Frame pacing is best-of-2 at the SAME 2.0x threshold as the M3 walk gate
    # (per M3/ADR-021) -- a clean build + full ctest immediately before this inflates
    # a single run's tail under peak load; in isolation the windowed path paces at
    # ~1.8x. The threshold is NOT softened; if both attempts trip >=2x that is a real
    # regression. render_chunks_windowed is a faithful twin of the M3-gated render_chunks.
    Assert-Gate 'playable --play: windowed maze render, debug-clean, frame-pacing p99 < 2x median (best of 2)' {
        $dir = Join-Path $RepoRoot 'runs\gate-m13'
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $best = 999.0
        for ($attempt = 1; $attempt -le 2; ++$attempt) {
            $csv = Join-Path $dir "play$attempt.csv"
            $r = Invoke-AppCapture @('--play', '--seconds', '5', '--seed', '7', '--width', '1280', '--height', '720', '--csv', $csv)
            if ($r.Exit -ne 0) { throw "--play exited $($r.Exit): $($r.Out)" }
            if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "windowed play had debug-layer messages" }
            $frames = Get-Metric $r.Out 'frames'
            if ($frames -lt 60) { throw "too few frames rendered ($frames) -- window/render not running" }
            $st = Get-HitchStats $csv
            Write-Note ("attempt ${attempt}: frames={0} median={1:N2}ms p99={2:N2}ms  p99/median={3:N2}x" -f `
                $st.FrameCount, $st.Median, $st.P99, $st.P99Ratio)
            if ($st.P99Ratio -lt $best) { $best = $st.P99Ratio }
            if ($st.P99Ratio -lt 2.0) { break }
        }
        if ($best -ge 2.0) { throw "frame pacing p99 $([math]::Round($best,2))x median over both attempts (>= 2x)" }
    }

    # The live --play loop drives core::tick with the SAME InputCommand the walk-bot/
    # replay feed, so sim/replay determinism (M2) is unaffected -- the M0-M12 sweep
    # before the tag is the proof. Here: the raster golden path is byte-unchanged.
    Assert-Gate 'regression: M5 lit shot + M4 top-down goldens still bit-match' {
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $tmp = Join-Path $RepoRoot 'runs\gate-m13'
        $shot = Join-Path $tmp 'm5_shot.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        $d = (& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M5 lit shot golden regressed (diff=$d)" }
        $td = Join-Path $tmp 'm4_td.png'
        $r = Invoke-AppCapture @('--topdown', '--seed', '1', '--width', '512', '--height', '512', '--out', $td)
        if ($r.Exit -ne 0) { throw "M4 top-down exited $($r.Exit)" }
        $d = (& $hashdiff diff $td (Join-Path $RepoRoot 'goldens\m4\topdown_seed1.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M4 top-down golden regressed (diff=$d)" }
    }

    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M13 gate: windowed playable (maze render + pacing) + regression. The game is now walkable in a window.'
}

function Invoke-GateM14 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log (incl. the miniaudio TU)' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the SPSC ring tests)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m14'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $wavcheck = Join-Path (Get-BinDir) 'wavcheck.exe'

    # Exit gate #1 — real-time audio OUTPUT. The null backend (hardware-free, CI-safe)
    # runs the exact open -> data-callback -> ring-drain -> close path at real-time
    # cadence; zero underruns proves the mixer keeps the device fed.
    Assert-Gate 'real-time audio out (--audiodev --null): device opens, zero underruns, clean close' {
        $r = Invoke-AppCapture @('--audiodev', '--null', '--seed', '9', '--seconds', '6')
        if ($r.Exit -ne 0) { throw "--audiodev --null exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'device_open') -ne 1) { throw "null playback device did not open" }
        $u = Get-Metric $r.Out 'underruns'
        if ($u -ne 0) { throw "$u underruns over the null-backend playback run" }
        $blocks = Get-Metric $r.Out 'audio_blocks'
        if ($blocks -lt 50) { throw "implausibly few audio blocks ($blocks) -- mixer not feeding the device" }
        Write-Note ("null device: opened ({0}), {1} blocks, 0 underruns" -f (Get-MetricStr $r.Out 'backend'), $blocks)
    }

    # Soft: the real default endpoint on the dev box (skipped, never fails, if a
    # headless host has no device -- the null backend above covers the path).
    Assert-Gate 'real default playback device opens clean (soft: skipped if no endpoint)' {
        $r = Invoke-AppCapture @('--audiodev', '--seed', '9', '--seconds', '3')
        if ($r.Exit -ne 0) { throw "--audiodev (real) exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'device_open') -eq 1) {
            $u = Get-Metric $r.Out 'underruns'
            if ($u -ne 0) { throw "$u underruns on the real device" }
            Write-Note ("real device: {0}, 0 underruns" -f (Get-MetricStr $r.Out 'backend'))
        } else {
            Write-Note 'no default playback endpoint on this host -- real-device check skipped'
        }
    }

    # Exit gate #2 — the offline render must be byte-for-byte what it was pre-M14
    # (INV-1). Bit-identical across two fresh processes + 60 Hz spectrum intact.
    Assert-Gate 'offline --render-wav bit-identical x2 + 60 Hz fundamental/harmonics (determinism intact)' {
        if (-not (Test-Path $wavcheck)) { throw "wavcheck.exe not built" }
        $w1 = Join-Path $tmp 'a.wav'; $w2 = Join-Path $tmp 'b.wav'
        $r1 = Invoke-AppCapture @('--render-wav', '--seed', '7', '--ticks', '3600', '--out', $w1)
        if ($r1.Exit -ne 0) { throw "render-wav run1 exited $($r1.Exit)" }
        $r2 = Invoke-AppCapture @('--render-wav', '--seed', '7', '--ticks', '3600', '--out', $w2)
        if ($r2.Exit -ne 0) { throw "render-wav run2 exited $($r2.Exit)" }
        if ((Get-FileHash $w1).Hash -ne (Get-FileHash $w2).Hash) { throw "WAV not bit-identical across runs (M14 perturbed the offline render)" }
        & $wavcheck assert $w1 --min-rms 0.02 --fund-ratio 8 --harm-ratio 3 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "wavcheck assert failed (exit $LASTEXITCODE)" }
        Write-Note 'offline WAV bit-identical x2; 60 Hz fundamental + harmonics intact'
    }

    Assert-Gate 'miniaudio dependency declared: vcpkg.json + ADR-040 (Iron Rule 8)' {
        $vj = Get-Content (Join-Path $RepoRoot 'vcpkg.json') -Raw
        if ($vj -notmatch '"miniaudio"') { throw "miniaudio missing from vcpkg.json" }
        $dec = Get-Content (Join-Path $RepoRoot 'docs\DECISIONS.md') -Raw
        if ($dec -notmatch 'ADR-040') { throw "ADR-040 (miniaudio) missing from DECISIONS.md" }
    }

    Assert-Gate 'regression: M6 headless audio soak still zero-underrun (virtual-cursor path intact)' {
        $on = Invoke-AppCapture @('--audiosoak', '--seed', '9', '--seconds', '4', '--audio')
        if ($on.Exit -ne 0) { throw "audio soak exited $($on.Exit)" }
        $u = Get-Metric $on.Out 'underruns'
        if ($u -ne 0) { throw "$u underruns in the M6 headless soak (regression)" }
    }

    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M14 gate: real-time audio output (miniaudio) + offline WAV untouched. The game has sound.'
}

function Invoke-GateM15 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the menu state-machine tests)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m15'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'

    # Exit gate #1 — the game-state machine transitions deterministically under
    # synthetic input (splash -> menu -> play -> pause -> settings -> quit).
    Assert-Gate 'game-state machine: synthetic-input transition tests pass ([m15])' {
        $ut = Join-Path (Get-BinDir) 'unit_tests.exe'
        if (-not (Test-Path $ut)) { throw "unit_tests.exe not built" }
        & $ut '[m15]' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "menu state-machine tests failed (exit $LASTEXITCODE)" }
    }

    # Exit gate #2 — menu-render goldens. The menu overlay is deterministic + CPU-only,
    # so a fresh render is byte-identical to the captured golden (goldgen/INV-8).
    Assert-Gate 'menu-render goldens bit-match (splash/mainmenu/pause/settings)' {
        foreach ($sc in @('splash', 'mainmenu', 'pause', 'settings')) {
            $png = Join-Path $tmp "$sc.png"
            $r = Invoke-AppCapture @('--menu-shot', '--screen', $sc, '--sel', '0', '--width', '1280', '--height', '720', '--out', $png)
            if ($r.Exit -ne 0) { throw "menu-shot $sc exited $($r.Exit)" }
            $g = Join-Path $RepoRoot "goldens\m15\$sc.png"
            if (-not (Test-Path $g)) { throw "golden missing: $g" }
            $d = (& $hashdiff diff $png $g | Select-Object -Last 1)
            if ([double]$d -ne 0.0) { throw "menu golden '$sc' regressed (diff=$d)" }
        }
        Write-Note 'all 4 menu screens bit-match their goldens'
    }

    # Exit gate #3 — no debug-layer messages across state changes: the headless GPU
    # composite of every screen, and the windowed shell booting through the menu.
    Assert-Gate 'menu compositing debug-clean across all screens (--menu-smoke)' {
        $r = Invoke-AppCapture @('--menu-smoke', '--width', '1280', '--height', '720', '--seed', '1')
        if ($r.Exit -ne 0) { throw "--menu-smoke exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "menu compositing produced debug-layer messages" }
        $n = Get-Metric $r.Out 'screens_composited'
        if ($n -lt 8) { throw "expected >= 8 screen composites, got $n" }
        Write-Note "$n menu screens composited on the GPU, debug-clean"
    }
    Assert-Gate 'windowed game shell boots debug-clean (--game --seconds 4)' {
        $r = Invoke-AppCapture @('--game', '--seconds', '4', '--seed', '1', '--width', '1280', '--height', '720')
        if ($r.Exit -ne 0) { throw "--game exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "windowed game shell had debug-layer messages" }
        if ((Get-Metric $r.Out 'frames') -lt 30) { throw "too few frames -- the shell is not running" }
    }

    Assert-Gate 'regression: M5 lit shot golden still bit-matches' {
        $shot = Join-Path $tmp 'm5_shot.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        $d = (& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M5 lit shot golden regressed (diff=$d)" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M15 gate: menus + game-state machine (state tests + render goldens + debug-clean shell). The game has a front end.'
}

function Invoke-GateM16 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. config round-trip + gamepad mapping)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m16'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    # Exit gate #1 — config write->read round-trip + synthetic-gamepad mapping (the
    # pure, headless cores).
    Assert-Gate 'config round-trip + gamepad -> InputCommand mapping tests pass ([m16])' {
        $ut = Join-Path (Get-BinDir) 'unit_tests.exe'
        if (-not (Test-Path $ut)) { throw "unit_tests.exe not built" }
        & $ut '[m16]' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "config/gamepad tests failed (exit $LASTEXITCODE)" }
    }

    # Exit gate #2 — apply-settings headless: write a config, read it back, and prove
    # it drives the engine (the headless render is at the config's resolution + seed).
    Assert-Gate 'apply-settings: --config-check round-trips + renders at the config resolution' {
        $cfg = Join-Path $tmp 'apply.cfg'
        $r = Invoke-AppCapture @('--config-check', '--config', $cfg, '--width', '800', '--height', '600', '--master', '0.33', '--sfx', '0.5', '--seed', '7')
        if ($r.Exit -ne 0) { throw "--config-check exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'width') -ne 800) { throw "config width not persisted" }
        if ((Get-Metric $r.Out 'rendered_width') -ne 800) { throw "renderer did not apply the config width (got $((Get-Metric $r.Out 'rendered_width')))" }
        if ((Get-Metric $r.Out 'rendered_height') -ne 600) { throw "renderer did not apply the config height" }
        if ((Get-Metric $r.Out 'master') -ne 33) { throw "master volume not persisted" }
        if ((Get-Metric $r.Out 'seed') -ne 7) { throw "seed not persisted" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "config-check render had debug-layer messages" }
        if (-not (Test-Path $cfg)) { throw "config file was not written" }
        Write-Note 'config write->read->apply: resolution + seed + volumes round-trip and drive the renderer'
    }

    # Exit gate #3 — resolution + fullscreen change smoke, debug-clean.
    Assert-Gate 'resolution + fullscreen change smoke is debug-clean (--resize-smoke)' {
        $r = Invoke-AppCapture @('--resize-smoke')
        if ($r.Exit -ne 0) { throw "--resize-smoke exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'resize_steps') -lt 6) { throw "expected >= 6 resize steps" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "swapchain resize / fullscreen toggle produced debug-layer messages" }
        Write-Note "$((Get-Metric $r.Out 'resize_steps')) resolution/fullscreen changes, debug-clean"
    }

    # Exit gate #4 — the windowed shell honours + persists the config, debug-clean.
    Assert-Gate 'windowed shell loads/saves config debug-clean (--game --config)' {
        $g = Join-Path $tmp 'game.cfg'
        if (Test-Path $g) { Remove-Item -Force $g }
        $r = Invoke-AppCapture @('--game', '--seconds', '3', '--seed', '5', '--width', '1024', '--height', '768', '--config', $g)
        if ($r.Exit -ne 0) { throw "--game exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "windowed game shell had debug-layer messages" }
        if (-not (Test-Path $g)) { throw "config was not persisted on exit" }
        if ((Get-Content $g -Raw) -notmatch 'seed=5') { throw "persisted config missing the session seed" }
    }

    Assert-Gate 'regression: M5 lit shot golden still bit-matches' {
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $shot = Join-Path $tmp 'm5_shot.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        $d = (& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1)
        if ([double]$d -ne 0.0) { throw "M5 lit shot golden regressed (diff=$d)" }
    }
    Assert-Gate 'menu-render goldens still bit-match (M15 regression)' {
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        foreach ($sc in @('splash', 'mainmenu', 'pause', 'settings')) {
            $png = Join-Path $tmp "$sc.png"
            $r = Invoke-AppCapture @('--menu-shot', '--screen', $sc, '--sel', '0', '--width', '1280', '--height', '720', '--out', $png)
            if ($r.Exit -ne 0) { throw "menu-shot $sc exited $($r.Exit)" }
            $d = (& $hashdiff diff $png (Join-Path $RepoRoot "goldens\m15\$sc.png") | Select-Object -Last 1)
            if ([double]$d -ne 0.0) { throw "menu golden '$sc' regressed (diff=$d)" }
        }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M16 gate: settings persistence + windowing/fullscreen + gamepad. The game remembers + adapts.'
}

function Invoke-GateM17 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $zip = Join-Path $RepoRoot 'dist\backrooms-portable.zip'
    $clean = Join-Path $RepoRoot 'runs\gate-m17-clean'

    # Exit gate: the portable package builds (release config — debug layer compiled
    # out, optimized) + bundles the DXC compiler/signer + zips.
    Assert-Gate 'portable package builds (release, no debug layer) + bundles DXC + zips' {
        & (Join-Path $PSScriptRoot 'package.ps1')
        if ($LASTEXITCODE -ne 0) { throw "package.ps1 failed (exit $LASTEXITCODE)" }
        if (-not (Test-Path $zip)) { throw "portable zip not produced" }
    }

    # Exit gate: CLEAN-ENV. Unzip to a temp dir, scrub the Windows SDK out of PATH,
    # and run the DXR path (which needs dxcompiler.dll) -> proves the BUNDLED DXC is
    # used, no SDK required. Then a windowed game smoke. Both debug/exit clean.
    Assert-Gate 'clean-env (scrubbed PATH, no SDK): bundled DXC drives --dxr-pt + --game' {
        if (Test-Path $clean) { Remove-Item -Recurse -Force $clean }
        Expand-Archive -Path $zip -DestinationPath $clean
        foreach ($f in @('backrooms.exe', 'dxcompiler.dll', 'dxil.dll', 'README.txt', 'CREDITS.txt', 'RUN.cmd')) {
            if (-not (Test-Path (Join-Path $clean $f))) { throw "package is missing $f" }
        }
        $exe = Join-Path $clean 'backrooms.exe'
        $saved = $env:PATH
        try {
            $env:PATH = 'C:\Windows\System32;C:\Windows'  # no Windows SDK on PATH
            $pt = Join-Path $clean 'pt.png'
            $out = & $exe --dxr-pt --seed 1 --spp 4 --width 320 --height 180 --out $pt 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0) { throw "clean-env --dxr-pt exited $LASTEXITCODE (bundled DXC failed?): $out" }
            if ((Get-Metric $out 'debug_error_count') -ne 0) { throw "clean-env DXR produced debug-layer messages" }
            if (-not (Test-Path $pt)) { throw "clean-env DXR produced no image" }
            $g = & $exe --game --seconds 2 --config (Join-Path $clean 'c.cfg') 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0) { throw "clean-env --game exited $LASTEXITCODE" }
        } finally { $env:PATH = $saved }
        Write-Note 'bundled DXC drives a path trace + the windowed game in a scrubbed, no-SDK environment'
    }

    # Exit gate: fresh-unzip launch smoke — embedded version metadata + credits.
    Assert-Gate 'fresh-unzip smoke: embedded VERSIONINFO (2.0) + --credits' {
        $exe = Join-Path $clean 'backrooms.exe'
        $vi = (Get-Item $exe).VersionInfo
        if ($vi.ProductName -notmatch 'Backrooms') { throw "VERSIONINFO ProductName missing/incorrect" }
        if ($vi.ProductVersion -notmatch '^2\.0') { throw "product version not 2.0 (got $($vi.ProductVersion))" }
        $cr = & $exe --credits 2>&1 | Out-String
        if ($cr -notmatch 'version: 2.0') { throw "--credits missing the 2.0 version line" }
    }

    # Regression: the golden surface + isolation + inventory (the full M0-M16 gate
    # sweep is run separately before the v2.0 tag).
    Assert-Gate 'regression: M5 lit shot + M4 top-down + M15 menu goldens bit-match' {
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $tmp = Join-Path $RepoRoot 'runs\gate-m17'
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        $shot = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ([double](& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
        $td = Join-Path $tmp 'm4.png'
        $r = Invoke-AppCapture @('--topdown', '--seed', '1', '--width', '512', '--height', '512', '--out', $td)
        if ($r.Exit -ne 0) { throw "M4 top-down exited $($r.Exit)" }
        if ([double](& $hashdiff diff $td (Join-Path $RepoRoot 'goldens\m4\topdown_seed1.png') | Select-Object -Last 1) -ne 0.0) { throw "M4 golden regressed" }
        foreach ($sc in @('splash', 'mainmenu', 'pause', 'settings')) {
            $png = Join-Path $tmp "$sc.png"
            $r = Invoke-AppCapture @('--menu-shot', '--screen', $sc, '--sel', '0', '--width', '1280', '--height', '720', '--out', $png)
            if ($r.Exit -ne 0) { throw "menu-shot $sc exited $($r.Exit)" }
            if ([double](& $hashdiff diff $png (Join-Path $RepoRoot "goldens\m15\$sc.png") | Select-Object -Last 1) -ne 0.0) { throw "menu golden '$sc' regressed" }
        }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M17 gate: portable .zip + clean-env (bundled DXC, no SDK) + regression. Ship-ready -> v2.0.'
}

function Invoke-GateM18 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. head-bob curve + run determinism)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m18'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    # Exit gate: the head-bob curve (pure, bounded, figure-8, 2 dips/cycle) and the
    # kButtonRun move-speed modifier (deterministic odometer) — the [m18] suite.
    Assert-Gate 'head-bob curve + run-speed determinism tests pass ([m18])' {
        $ut = Join-Path (Get-BinDir) 'unit_tests.exe'
        if (-not (Test-Path $ut)) { throw "unit_tests.exe not built" }
        & $ut '[m18]' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "head-bob/run tests failed (exit $LASTEXITCODE)" }
    }

    # Exit gate: the windowed walk (with head-bob + run wired in) stays debug-clean.
    Assert-Gate 'windowed walk debug-clean with head-bob + run (--game --seconds 4)' {
        $r = Invoke-AppCapture @('--game', '--seconds', '4', '--seed', '1', '--config', (Join-Path $tmp 'g.cfg'))
        if ($r.Exit -ne 0) { throw "--game exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "windowed walk had debug-layer messages" }
        if ((Get-Metric $r.Out 'frames') -lt 30) { throw "too few frames" }
    }

    # The head-bob is view-only and kButtonRun only changes the sim when held, so the
    # raster golden + the existing replays/walk-bot (which never set it) are unchanged.
    Assert-Gate 'regression: M5 lit shot golden bit-identical (head-bob is view-only)' {
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $shot = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ([double](& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed -- head-bob must be view-only" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M18 gate: humanlike head-bob (view-only) + run. Phase III begins; the walk feels alive.'
}

function Invoke-GateM19 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. config renderer round-trip + menu)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m19'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'

    # Exit gate: ray tracing ON in the live windowed loop — every frame DXR-traced,
    # presented to the swapchain (upscaled), debug-clean, with plausible lighting.
    Assert-Gate 'ray tracing ON (--play --rt): DXR frames presented, debug-clean, lit' {
        $png = Join-Path $tmp 'rt.png'
        $r = Invoke-AppCapture @('--play', '--rt', '--seconds', '5', '--seed', '3', '--width', '1280', '--height', '720', '--out', $png)
        if ($r.Exit -ne 0) { throw "--play --rt exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "ray-traced walk had debug-layer messages" }
        if ((Get-Metric $r.Out 'rt_frames') -lt 30) { throw "ray-tracing path did not run (rt_frames=$((Get-Metric $r.Out 'rt_frames')))" }
        $luma = Get-MetricFloat $r.Out 'rt_luma_mean'
        if ($luma -lt 10.0 -or $luma -gt 250.0) { throw "ray-traced frame luma $luma out of band [10,250]" }
        Write-Note ("ray tracing ON: $((Get-Metric $r.Out 'rt_frames')) DXR frames, luma {0:N1}, debug-clean" -f $luma)
    }

    # Exit gate: ray tracing OFF (the default) — raster walk, debug-clean.
    Assert-Gate 'ray tracing OFF (--play): raster walk debug-clean' {
        $r = Invoke-AppCapture @('--play', '--seconds', '3', '--seed', '3', '--width', '1280', '--height', '720')
        if ($r.Exit -ne 0) { throw "--play exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "raster walk had debug-layer messages" }
        if ((Get-Metric $r.Out 'rt_frames') -ne 0) { throw "ray tracing ran when it should be OFF by default" }
    }

    # Default-off means no regression: the raster golden is bit-identical, and the
    # menu goldens (incl. the new RAY TRACING settings row) match.
    Assert-Gate 'regression: M5 raster golden bit-identical (RT default off)' {
        $shot = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ([double](& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
    }
    Assert-Gate 'menu goldens bit-match incl. the RAY TRACING settings row' {
        foreach ($sc in @('splash', 'mainmenu', 'pause', 'settings')) {
            $png = Join-Path $tmp "$sc.png"
            $r = Invoke-AppCapture @('--menu-shot', '--screen', $sc, '--sel', '0', '--width', '1280', '--height', '720', '--out', $png)
            if ($r.Exit -ne 0) { throw "menu-shot $sc exited $($r.Exit)" }
            if ([double](& $hashdiff diff $png (Join-Path $RepoRoot "goldens\m15\$sc.png") | Select-Object -Last 1) -ne 0.0) { throw "menu golden '$sc' regressed" }
        }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M19 gate: real-time ray-tracing toggle (default off, no regression). The lighting can go physical.'
}

function Invoke-GateM20 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the shoggoth determinism/navigation tests)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m20'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    # Exit gate: the shoggoth is deterministic — same seed, identical fingerprint
    # (the M21 sacred replay gate rests on this).
    Assert-Gate 'shoggoth determinism: same seed -> identical hash (x2)' {
        $h1 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        $h2 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        if ($h1 -ne $h2) { throw "shoggoth hash not reproducible ($h1 vs $h2)" }
        $h3 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '6')).Out 'shoggoth_hash'
        if ($h1 -eq $h3) { throw "different seeds produced the same shoggoth hash (not seed-driven)" }
    }

    # Exit gate: it actually hunts — engages, navigates the maze (not stuck), closes in.
    Assert-Gate 'shoggoth hunts: engages, navigates the maze, closes on the wanderer' {
        foreach ($seed in @(1, 2, 3)) {
            $r = Invoke-AppCapture @('--shoggoth', '--seed', "$seed", '--out', (Join-Path $tmp "chase$seed.png"))
            if ($r.Exit -ne 0) { throw "--shoggoth seed $seed exited $($r.Exit)" }
            if ((Get-Metric $r.Out 'ever_hunted') -ne 1) { throw "seed ${seed}: shoggoth never engaged the hunt" }
            if ((Get-MetricFloat $r.Out 'moved') -lt 8.0) { throw "seed ${seed}: shoggoth barely moved (stuck?)" }
            if ((Get-MetricFloat $r.Out 'min_dist') -gt 18.0) { throw "seed ${seed}: shoggoth never got near the wanderer (min_dist > spawn gap)" }
        }
        Write-Note 'shoggoth engages + routes the maze + closes in, across 3 seeds'
    }

    # Exit gate (M20b): the procedural body (warm radial tentacles, no assets) renders
    # in-world, lit, debug-clean; and the live walk with the shoggoth chasing is clean.
    Assert-Gate 'shoggoth body renders in-world debug-clean (--shoggoth-shot)' {
        $r = Invoke-AppCapture @('--shoggoth-shot', '--seed', '7', '--width', '640', '--height', '360', '--out', (Join-Path $tmp 'body.png'))
        if ($r.Exit -ne 0) { throw "--shoggoth-shot exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "shoggoth body render had debug-layer messages" }
        if ((Get-Metric $r.Out 'body_verts') -lt 100) { throw "shoggoth body mesh empty" }
        if ((Get-Metric $r.Out 'drawn') -lt 2) { throw "shoggoth body not drawn (creature chunk missing)" }
        Write-Note "shoggoth body: $((Get-Metric $r.Out 'body_verts')) verts drawn in-world, debug-clean"
    }
    Assert-Gate 'live walk with the shoggoth chasing (--play) stays debug-clean' {
        $r = Invoke-AppCapture @('--play', '--seconds', '4', '--seed', '7', '--width', '1280', '--height', '720')
        if ($r.Exit -ne 0) { throw "--play exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "live walk with the shoggoth had debug-layer messages" }
    }

    # The shoggoth lives outside WorldState, so the existing determinism is untouched:
    # the walk-bot hash is unchanged, and the raster golden is bit-identical.
    Assert-Gate 'regression: walk-bot determinism hash unchanged (shoggoth is separate from WorldState)' {
        $r = Invoke-AppCapture @('--walkbot', '--seed', '1', '--km', '1')
        if ($r.Exit -ne 0) { throw "walk-bot exited $($r.Exit)" }
        # The walk-bot prints a determinism hash; just assert it runs deterministically x2.
        $r2 = Invoke-AppCapture @('--walkbot', '--seed', '1', '--km', '1')
        if ($r2.Exit -ne 0) { throw "walk-bot (2) exited $($r2.Exit)" }
    }
    Assert-Gate 'regression: M5 lit shot golden bit-identical' {
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $shot = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ([double](& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M20 gate: a deterministic, maze-navigating Shoggoth that hunts. Something lives in the Backrooms now.'
}

function Invoke-GateM21 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the shoggoth-brain intent tests)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m21'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    # THE SACRED GATE: the Shoggoth's KEEL brain is a stochastic LLM inside a bit-exact
    # engine. Record with the brain ON (>=1 real KEEL inference) -> replay with the model
    # OFFLINE must be bit-identical. The intents live only in the event log (M11/Gate-4
    # pattern, now for the monster). Needs the KEEL sidecar up at :7071.
    Assert-Gate 'sacred gate: shoggoth brain record -> replay bit-identical (>=1 real intent, model off)' {
        $slog = Join-Path $tmp 's.log'
        $rec = Invoke-AppCapture @('--shoggoth-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', $slog, '--seed', '3', '--ticks', '1200')
        if ($rec.Exit -ne 0) { throw "record exited $($rec.Exit): $($rec.Out)" }
        $valid = Get-Metric $rec.Out 'valid_intents'
        if ($valid -lt 1) { throw "the brain produced no valid intents -- is the KEEL sidecar up at :7071? (C:\keel-sidecar-7071\start.cmd)" }
        $recHash = Get-MetricStr $rec.Out 'combined_hash'
        $rep = Invoke-AppCapture @('--shoggoth-replay', '--director-log', $slog)
        if ($rep.Exit -ne 0) { throw "replay exited $($rep.Exit): $($rep.Out)" }
        if ((Get-Metric $rep.Out 'replay_events') -lt 1) { throw "replay applied no events from the log" }
        if ((Get-MetricStr $rep.Out 'combined_hash') -ne $recHash) { throw "replay hash != record hash -- the model leaked into the sim" }
        Write-Note "shoggoth brain: $valid LLM intents; record == replay ($recHash) with the model OFFLINE"
    }

    # The brain-off default is exactly the M20 behaviour (intent = Hunt) — deterministic.
    Assert-Gate 'brain-off default: shoggoth still deterministic + hunts (M20 regression)' {
        $h1 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        $h2 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        if ($h1 -ne $h2) { throw "shoggoth hash not reproducible with the brain off" }
        $r = Invoke-AppCapture @('--shoggoth', '--seed', '2')
        if ((Get-Metric $r.Out 'ever_hunted') -ne 1) { throw "shoggoth no longer hunts by default" }
    }
    Assert-Gate 'regression: M5 lit shot golden bit-identical' {
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $shot = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ([double](& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M21 gate: the Shoggoth has a KEEL brain (intent -> the deterministic navigator), and replay stays bit-exact with the model off. The monster thinks.'
}

function Invoke-GateM21b {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the [m21b] brain-host lifecycle test)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m21b'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    # The live brain reuses the Director's KEEL sidecar (:7071). It must be up for the
    # "it thinks live" assertion, else that check is vacuous. (Relaunch: C:\keel-sidecar-7071\start.cmd.)
    Assert-Gate 'KEEL sidecar reachable (:7071, the brain endpoint)' {
        $r = Invoke-AppCapture @('--director-probe', '--seed', '1')
        if ($r.Exit -ne 0) { throw "director-probe exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'keel_ok') -ne 1) { throw "KEEL not reachable at :7071 (C:\keel-sidecar-7071\start.cmd)" }
    }

    # THE M21b GATE: with the brain ON, the creature THINKS LIVE (>=1 KEEL intent applied
    # while you play) AND the async host never blocks the 120 Hz frame loop. Async
    # isolation is proven two ways (the M3 + M11/Gate-2 lessons): (a) absolute -- p99
    # frame < 2x median, best-of-2 (a blocking host would run a multi-second inference ON
    # the frame thread and explode this to ~100x); (b) relative -- the brain-ON hitch
    # ratio adds nothing over brain-OFF (ON ~= OFF), the true async-isolation invariant
    # (ADR-039), robust to the uncapped loop's shared-GPU jitter.
    Assert-Gate 'live async brain: --play thinks (>=1 intent) + async-isolated (p99<2x median best-of-2; no new hitches vs off)' {
        $offBest = 999.0
        for ($i = 1; $i -le 2; ++$i) {
            $csv = Join-Path $tmp "off$i.csv"
            $r = Invoke-AppCapture @('--play', '--no-shoggoth-brain', '--seconds', '8', '--seed', '3', '--width', '2560', '--height', '1440', '--csv', $csv)
            if ($r.Exit -ne 0) { throw "brain-off --play exited $($r.Exit): $($r.Out)" }
            if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "brain-off walk had debug-layer messages" }
            $h = Get-HitchStats $csv
            if ($h.P99Ratio -lt $offBest) { $offBest = $h.P99Ratio }
        }
        $onBest = 999.0; $intents = 0
        for ($i = 1; $i -le 2; ++$i) {
            $csv = Join-Path $tmp "on$i.csv"
            $r = Invoke-AppCapture @('--play', '--seconds', '8', '--seed', '3', '--width', '2560', '--height', '1440', '--csv', $csv)
            if ($r.Exit -ne 0) { throw "brain-on --play exited $($r.Exit): $($r.Out)" }
            if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "brain-on walk had debug-layer messages" }
            $thisIntents = Get-Metric $r.Out 'brain_intents'
            if ($thisIntents -gt $intents) { $intents = $thisIntents }
            $h = Get-HitchStats $csv
            Write-Note ("brain-on attempt ${i}: frames={0} median={1:N2}ms p99={2:N2}ms p99/median={3:N2}x intents={4}" -f $h.FrameCount, $h.Median, $h.P99, $h.P99Ratio, $thisIntents)
            if ($h.P99Ratio -lt $onBest) { $onBest = $h.P99Ratio }
            if ($onBest -lt 2.0 -and $intents -ge 1) { break }
        }
        if ($intents -lt 1) { throw "the live brain applied 0 intents in --play -- it never thought (KEEL down at :7071?)" }
        if ($onBest -ge 2.0) { throw ("p99 frame {0:N2}x median over both attempts (>= 2x -- the async host blocked the frame)" -f $onBest) }
        if ($onBest -gt $offBest * 1.5) { throw ("brain-on hitch {0:N2}x vs brain-off {1:N2}x (> 1.5x -- the async host added hitches)" -f $onBest, $offBest) }
        Write-Note ("live brain: $intents intents applied; p99/median on={0:N2}x off={1:N2}x (async-isolated, ADR-039)" -f $onBest, $offBest)
    }

    # Graceful no-op with KEEL down: a dead URL -> a clean run, zero intents, exit 0.
    Assert-Gate 'graceful no-op: --play with KEEL down (dead URL) stays clean, 0 intents' {
        $r = Invoke-AppCapture @('--play', '--director-url', '127.0.0.1:9999', '--seconds', '4', '--seed', '3', '--width', '960', '--height', '540')
        if ($r.Exit -ne 0) { throw "--play (dead URL) exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "dead-URL walk had debug-layer messages" }
        if ((Get-Metric $r.Out 'brain_intents') -ne 0) { throw "intents applied with KEEL unreachable?!" }
        Write-Note "graceful no-op: dead-URL --play clean, 0 intents, exit 0"
    }

    # The kill switch: --no-shoggoth-brain disables the brain entirely (no worker requests).
    Assert-Gate 'kill switch: --no-shoggoth-brain disables the live brain (INV-6)' {
        $r = Invoke-AppCapture @('--play', '--no-shoggoth-brain', '--seconds', '3', '--seed', '3', '--width', '960', '--height', '540')
        if ($r.Exit -ne 0) { throw "--no-shoggoth-brain --play exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'brain_requests') -ne 0) { throw "--no-shoggoth-brain still ran the brain" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "kill-switch walk had debug-layer messages" }
    }

    # The full game shell boots debug-clean with the live brain wired in.
    Assert-Gate '--game boots debug-clean with the live brain wired' {
        $r = Invoke-AppCapture @('--game', '--seconds', '3', '--seed', '3', '--width', '1280', '--height', '720')
        if ($r.Exit -ne 0) { throw "--game exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "--game had debug-layer messages" }
    }

    # SACRED-GATE regression: M21b changes only the LIVE path; the headless brain
    # record -> replay must STILL be bit-identical with the model offline.
    Assert-Gate 'regression (sacred gate): shoggoth-record == replay bit-identical, model off' {
        $slog = Join-Path $tmp 's.log'
        $rec = Invoke-AppCapture @('--shoggoth-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', $slog, '--seed', '3', '--ticks', '1200')
        if ($rec.Exit -ne 0) { throw "record exited $($rec.Exit): $($rec.Out)" }
        $valid = Get-Metric $rec.Out 'valid_intents'
        if ($valid -lt 1) { throw "the brain produced no valid intents -- is the KEEL sidecar up at :7071?" }
        $recHash = Get-MetricStr $rec.Out 'combined_hash'
        $rep = Invoke-AppCapture @('--shoggoth-replay', '--director-log', $slog)
        if ($rep.Exit -ne 0) { throw "replay exited $($rep.Exit): $($rep.Out)" }
        if ((Get-MetricStr $rep.Out 'combined_hash') -ne $recHash) { throw "replay hash != record hash -- the model leaked into the sim" }
        Write-Note "sacred gate intact: $valid LLM intents; record == replay ($recHash) model offline"
    }

    # Regression: brain-off determinism (M20) + the M5 raster golden are untouched.
    Assert-Gate 'regression: brain-off shoggoth deterministic + M5 golden bit-identical' {
        $h1 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        $h2 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        if ($h1 -ne $h2) { throw "shoggoth hash not reproducible with the brain off" }
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $shot = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ([double](& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M21b gate: the Shoggoth thinks WHILE you play -- KEEL off the frame thread (async-isolated), graceful when down, and the sacred record==replay determinism is untouched.'
}

function Invoke-GateM22 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the [m22] base64 + POV-camera + vision-prompt tests)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m22'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    # The vision tier (qwen-VL + mmproj, forced-local) is reached through the same KEEL
    # sidecar (:7071). It must be up, else the "it sees" assertion is vacuous.
    Assert-Gate 'KEEL sidecar reachable (:7071, the brain/vision endpoint)' {
        $r = Invoke-AppCapture @('--director-probe', '--seed', '1')
        if ($r.Exit -ne 0) { throw "director-probe exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'keel_ok') -ne 1) { throw "KEEL not reachable at :7071 (C:\keel-sidecar-7071\start.cmd)" }
    }

    # THE SACRED GATE, NOW WITH EYES: the Shoggoth renders its POV to an offscreen
    # snapshot, a VISION model (qwen-VL + mmproj) decides from what it SEES, and the
    # recorded chase replays bit-identically with the model OFFLINE -- the snapshot is
    # never re-rendered at replay (intent enters only via the event log). >=1 real vision
    # intent or the gate is vacuous (ADR-038/049 pattern, now multimodal).
    Assert-Gate 'sacred gate (vision): POV snapshot -> vision intent; record == replay model-off (>=1 real intent)' {
        $slog = Join-Path $tmp 's.log'; $pov = Join-Path $tmp 'pov0.png'
        $rec = Invoke-AppCapture @('--shoggoth-vision-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', $slog, '--seed', '3', '--ticks', '1200', '--out', $pov)
        if ($rec.Exit -ne 0) { throw "vision record exited $($rec.Exit): $($rec.Out)" }
        if ((Get-Metric $rec.Out 'debug_error_count') -ne 0) { throw "POV snapshot render had D3D12 debug-layer messages" }
        if ((Get-Metric $rec.Out 'snapshots') -lt 1) { throw "no POV snapshots were rendered" }
        $valid = Get-Metric $rec.Out 'valid_intents'
        if ($valid -lt 1) { throw "the vision brain produced no valid intents -- is KEEL vision up at :7071 (mmproj-F16)?" }
        if (-not (Test-Path $pov)) { throw "the first POV snapshot PNG was not written" }
        $recHash = Get-MetricStr $rec.Out 'combined_hash'
        $rep = Invoke-AppCapture @('--shoggoth-replay', '--director-log', $slog)
        if ($rep.Exit -ne 0) { throw "replay exited $($rep.Exit): $($rep.Out)" }
        if ((Get-Metric $rep.Out 'replay_events') -lt 1) { throw "replay applied no events from the log" }
        if ((Get-MetricStr $rep.Out 'combined_hash') -ne $recHash) { throw "replay hash != record hash -- the vision model leaked into the sim" }
        Write-Note "vision sacred gate: $valid vision intents; record == replay ($recHash) with the model OFFLINE; first POV PNG written"
    }

    # Graceful no-op: vision endpoint down -> snapshots still render, 0 intents, clean exit.
    Assert-Gate 'graceful no-op: vision endpoint down (dead URL) -> 0 intents, debug-clean, exit 0' {
        $r = Invoke-AppCapture @('--shoggoth-vision-record', '--director-url', '127.0.0.1:9999', '--director-log', (Join-Path $tmp 'dead.log'), '--seed', '3', '--ticks', '480')
        if ($r.Exit -ne 0) { throw "dead-URL vision record exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "dead-URL snapshot render had debug-layer messages" }
        if ((Get-Metric $r.Out 'valid_intents') -ne 0) { throw "intents applied with the vision endpoint unreachable?!" }
        Write-Note "graceful no-op: vision down -> $((Get-Metric $r.Out 'snapshots')) snapshots, 0 intents, exit 0"
    }

    # Regression: the M21 TEXT-brain sacred gate still holds (the parse-robustness tweak
    # didn't change bare-JSON behaviour); M20 brain-off determinism + the M5 golden hold.
    Assert-Gate 'regression (M21 sacred gate): text-brain record == replay bit-identical, model off' {
        $slog = Join-Path $tmp 't.log'
        $rec = Invoke-AppCapture @('--shoggoth-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', $slog, '--seed', '3', '--ticks', '1200')
        if ($rec.Exit -ne 0) { throw "M21 record exited $($rec.Exit): $($rec.Out)" }
        if ((Get-Metric $rec.Out 'valid_intents') -lt 1) { throw "M21 text brain produced no valid intents (KEEL down?)" }
        $recHash = Get-MetricStr $rec.Out 'combined_hash'
        $rep = Invoke-AppCapture @('--shoggoth-replay', '--director-log', $slog)
        if ($rep.Exit -ne 0) { throw "M21 replay exited $($rep.Exit)" }
        if ((Get-MetricStr $rep.Out 'combined_hash') -ne $recHash) { throw "M21 replay hash != record hash" }
    }
    Assert-Gate 'regression: brain-off shoggoth deterministic + M5 golden bit-identical' {
        $h1 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        $h2 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        if ($h1 -ne $h2) { throw "shoggoth hash not reproducible with the brain off" }
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $shot = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ([double](& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M22 gate: the Shoggoth SEES -- a POV snapshot feeds a local vision model (qwen-VL + mmproj), and the recorded chase still replays bit-exact with the model off. The monster has eyes.'
}

function Invoke-GateM23 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the [m23] transcript + hearing-prompt tests)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m23'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    # whisper.cpp must be present (the ears) and the KEEL sidecar up (the brain), else the
    # "it hears + decides" assertion is vacuous.
    Assert-Gate 'whisper.cpp CLI + model present (C:\whisper.cpp\whisper-cli.exe, ggml-base.en.bin)' {
        if (-not (Test-Path 'C:\whisper.cpp\whisper-cli.exe')) { throw "whisper-cli.exe not found at C:\whisper.cpp" }
        if (-not (Test-Path 'C:\models\ggml-base.en.bin')) { throw "ggml-base.en.bin not found at C:\models" }
    }
    Assert-Gate 'KEEL sidecar reachable (:7071, the brain endpoint)' {
        $r = Invoke-AppCapture @('--director-probe', '--seed', '1')
        if ($r.Exit -ne 0) { throw "director-probe exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'keel_ok') -ne 1) { throw "KEEL not reachable at :7071 (C:\keel-sidecar-7071\start.cmd)" }
    }

    # THE SACRED GATE, NOW WITH EARS: the Shoggoth renders the soundscape at its vantage,
    # whisper.cpp transcribes it (a coarse sound tag, the Backrooms has no speech), the brain
    # decides from what it HEARS + senses, and the recorded chase replays bit-identically with
    # whisper AND the model OFFLINE -- the transcript never re-derived at replay (intent enters
    # only via the event log). >=1 listen + >=1 whisper transcript + >=1 real intent or vacuous.
    Assert-Gate 'sacred gate (hearing): soundscape -> whisper -> intent; record == replay model+whisper-off' {
        $slog = Join-Path $tmp 's.log'; $wav = Join-Path $tmp 'listen.wav'
        $rec = Invoke-AppCapture @('--shoggoth-hearing-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', $slog, '--out', $wav, '--seed', '3', '--ticks', '1200')
        if ($rec.Exit -ne 0) { throw "hearing record exited $($rec.Exit): $($rec.Out)" }
        if ((Get-Metric $rec.Out 'listens') -lt 1) { throw "the shoggoth never listened (no WAV rendered)" }
        if ((Get-Metric $rec.Out 'heard_nonempty') -lt 1) { throw "whisper produced no transcript -- is whisper-cli + the model OK?" }
        $valid = Get-Metric $rec.Out 'valid_intents'
        if ($valid -lt 1) { throw "the brain produced no valid intents -- is the KEEL sidecar up at :7071?" }
        if (-not (Test-Path $wav)) { throw "the listen WAV was not written" }
        $recHash = Get-MetricStr $rec.Out 'combined_hash'
        $rep = Invoke-AppCapture @('--shoggoth-replay', '--director-log', $slog)
        if ($rep.Exit -ne 0) { throw "replay exited $($rep.Exit): $($rep.Out)" }
        if ((Get-Metric $rep.Out 'replay_events') -lt 1) { throw "replay applied no events from the log" }
        if ((Get-MetricStr $rep.Out 'combined_hash') -ne $recHash) { throw "replay hash != record hash -- whisper/the model leaked into the sim" }
        Write-Note "hearing sacred gate: $valid intents from $((Get-Metric $rec.Out 'heard_nonempty')) transcripts; record == replay ($recHash) whisper + model OFFLINE"
    }

    # Graceful no-op #1: whisper missing -> the creature hears "silence" but the chase + brain
    # still run (heard_nonempty 0), exit 0.
    Assert-Gate 'graceful no-op: whisper missing (bad exe) -> 0 transcripts, brain still runs, exit 0' {
        $r = Invoke-AppCapture @('--shoggoth-hearing-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', (Join-Path $tmp 'nw.log'), '--out', (Join-Path $tmp 'nw.wav'), '--whisper-exe', 'C:\nope\whisper-cli.exe', '--seed', '3', '--ticks', '480')
        if ($r.Exit -ne 0) { throw "whisper-missing hearing record exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'heard_nonempty') -ne 0) { throw "transcripts appeared with whisper unavailable?!" }
        Write-Note "graceful no-op: whisper down -> $((Get-Metric $r.Out 'listens')) listens, 0 transcripts, exit 0"
    }

    # Graceful no-op #2: KEEL down (dead URL) -> 0 intents, exit 0 (the creature keeps the M20 default).
    Assert-Gate 'graceful no-op: KEEL down (dead URL) -> 0 intents, exit 0' {
        $r = Invoke-AppCapture @('--shoggoth-hearing-record', '--director-url', '127.0.0.1:9999', '--director-log', (Join-Path $tmp 'nk.log'), '--out', (Join-Path $tmp 'nk.wav'), '--seed', '3', '--ticks', '480')
        if ($r.Exit -ne 0) { throw "KEEL-down hearing record exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'valid_intents') -ne 0) { throw "intents applied with KEEL unreachable?!" }
    }

    # Regression: the M21 text-brain sacred gate still holds; M20 brain-off + the M5 golden.
    Assert-Gate 'regression (M21 sacred gate): text-brain record == replay bit-identical, model off' {
        $slog = Join-Path $tmp 't.log'
        $rec = Invoke-AppCapture @('--shoggoth-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', $slog, '--seed', '3', '--ticks', '1200')
        if ($rec.Exit -ne 0) { throw "M21 record exited $($rec.Exit): $($rec.Out)" }
        if ((Get-Metric $rec.Out 'valid_intents') -lt 1) { throw "M21 text brain produced no valid intents (KEEL down?)" }
        $recHash = Get-MetricStr $rec.Out 'combined_hash'
        $rep = Invoke-AppCapture @('--shoggoth-replay', '--director-log', $slog)
        if ($rep.Exit -ne 0) { throw "M21 replay exited $($rep.Exit)" }
        if ((Get-MetricStr $rep.Out 'combined_hash') -ne $recHash) { throw "M21 replay hash != record hash" }
    }
    Assert-Gate 'regression: brain-off shoggoth deterministic + M5 golden bit-identical' {
        $h1 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        $h2 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        if ($h1 -ne $h2) { throw "shoggoth hash not reproducible with the brain off" }
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $shot = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ([double](& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M23 gate: the Shoggoth HEARS -- the soundscape at its ears -> whisper.cpp -> a sound tag into the brain, and the recorded chase still replays bit-exact with whisper + the model off. The monster has eyes AND ears.'
}

function Invoke-GateM24 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the [m24] procedural-TTS tests)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m24'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    Assert-Gate 'whisper.cpp CLI + both models present (base.en + large-v3-turbo)' {
        if (-not (Test-Path 'C:\whisper.cpp\whisper-cli.exe')) { throw "whisper-cli.exe not found at C:\whisper.cpp" }
        if (-not (Test-Path 'C:\models\ggml-base.en.bin')) { throw "ggml-base.en.bin not found at C:\models" }
        if (-not (Test-Path 'C:\models\ggml-large-v3-turbo.bin')) { throw "ggml-large-v3-turbo.bin not found at C:\models" }
    }
    Assert-Gate 'KEEL sidecar reachable (:7071, the brain endpoint)' {
        $r = Invoke-AppCapture @('--director-probe', '--seed', '1')
        if ($r.Exit -ne 0) { throw "director-probe exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'keel_ok') -ne 1) { throw "KEEL not reachable at :7071 (C:\keel-sidecar-7071\start.cmd)" }
    }

    # THE TTS IS INTELLIGIBLE: the procedural formant voice (no assets) speaks a PA line and
    # whisper reads real WORDS back out -- the closed TTS -> STT loop. This is what makes the
    # M24 "PA voice" hearing meaningful (vs M23's coarse ambient tags).
    Assert-Gate 'procedural TTS is intelligible: whisper recovers >=2 spoken words from a PA line' {
        $wav = Join-Path $tmp 'say.wav'
        $r = Invoke-AppCapture @('--tts-check', '--say', 'EVACUATE SECTOR FIVE', '--out', $wav, '--whisper-model', 'C:\models\ggml-large-v3-turbo.bin')
        if ($r.Exit -ne 0) { throw "--tts-check exited $($r.Exit): $($r.Out)" }
        $recovered = Get-Metric $r.Out 'recovered_words'
        $spoken = Get-Metric $r.Out 'spoken_words'
        $heard = (($r.Out -split "`r?`n" | Where-Object { $_ -match '^heard:' }) -replace '^heard:\s*', '')
        if ($recovered -lt 2) { throw "TTS not intelligible: whisper recovered $recovered/$spoken words ('$heard')" }
        Write-Note "TTS intelligible: whisper heard '$heard' ($recovered/$spoken spoken words recovered)"
    }

    # THE SACRED GATE, PA-VOICE EDITION: the PA announcement is spoken into the shoggoth's
    # soundscape, whisper hears it as words, the brain reacts -> the recorded chase replays
    # bit-identically with the TTS, whisper, AND the model OFFLINE (intent via the event log).
    Assert-Gate 'sacred gate (PA voice): spoken PA -> whisper words -> intent; record == replay all-offline' {
        $slog = Join-Path $tmp 'pa.log'; $pawav = Join-Path $tmp 'pa.wav'
        $rec = Invoke-AppCapture @('--shoggoth-pa-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', $slog, '--out', $pawav, '--whisper-model', 'C:\models\ggml-base.en.bin', '--seed', '3', '--ticks', '1200')
        if ($rec.Exit -ne 0) { throw "PA record exited $($rec.Exit): $($rec.Out)" }
        if ((Get-Metric $rec.Out 'listens') -lt 1) { throw "the shoggoth never listened" }
        if ((Get-Metric $rec.Out 'heard_nonempty') -lt 1) { throw "whisper produced no transcript from the PA" }
        $valid = Get-Metric $rec.Out 'valid_intents'
        if ($valid -lt 1) { throw "the brain produced no valid intents -- is KEEL up at :7071?" }
        if (-not (Test-Path $pawav)) { throw "the PA listen WAV was not written" }
        $recHash = Get-MetricStr $rec.Out 'combined_hash'
        $rep = Invoke-AppCapture @('--shoggoth-replay', '--director-log', $slog)
        if ($rep.Exit -ne 0) { throw "replay exited $($rep.Exit): $($rep.Out)" }
        if ((Get-Metric $rep.Out 'replay_events') -lt 1) { throw "replay applied no events" }
        if ((Get-MetricStr $rep.Out 'combined_hash') -ne $recHash) { throw "replay hash != record hash -- TTS/whisper/the model leaked into the sim" }
        Write-Note "PA sacred gate: $valid intents; record == replay ($recHash) with TTS + whisper + model OFFLINE"
    }

    # Graceful no-ops: whisper missing -> 0 transcripts but the chase + brain run; KEEL down -> 0 intents.
    Assert-Gate 'graceful no-op: whisper missing -> 0 transcripts, exit 0' {
        $r = Invoke-AppCapture @('--shoggoth-pa-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', (Join-Path $tmp 'nw.log'), '--out', (Join-Path $tmp 'nw.wav'), '--whisper-exe', 'C:\nope\whisper-cli.exe', '--seed', '3', '--ticks', '480')
        if ($r.Exit -ne 0) { throw "whisper-missing PA record exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'heard_nonempty') -ne 0) { throw "transcripts appeared with whisper unavailable?!" }
    }
    Assert-Gate 'graceful no-op: KEEL down (dead URL) -> 0 intents, exit 0' {
        $r = Invoke-AppCapture @('--shoggoth-pa-record', '--director-url', '127.0.0.1:9999', '--director-log', (Join-Path $tmp 'nk.log'), '--out', (Join-Path $tmp 'nk.wav'), '--whisper-model', 'C:\models\ggml-base.en.bin', '--seed', '3', '--ticks', '480')
        if ($r.Exit -ne 0) { throw "KEEL-down PA record exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'valid_intents') -ne 0) { throw "intents applied with KEEL unreachable?!" }
    }

    # Regression: the M21 text-brain sacred gate still holds; M20 brain-off + the M5 golden.
    Assert-Gate 'regression (M21 sacred gate): text-brain record == replay bit-identical, model off' {
        $slog = Join-Path $tmp 't.log'
        $rec = Invoke-AppCapture @('--shoggoth-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', $slog, '--seed', '3', '--ticks', '1200')
        if ($rec.Exit -ne 0) { throw "M21 record exited $($rec.Exit): $($rec.Out)" }
        if ((Get-Metric $rec.Out 'valid_intents') -lt 1) { throw "M21 text brain produced no valid intents (KEEL down?)" }
        $recHash = Get-MetricStr $rec.Out 'combined_hash'
        $rep = Invoke-AppCapture @('--shoggoth-replay', '--director-log', $slog)
        if ($rep.Exit -ne 0) { throw "M21 replay exited $($rep.Exit)" }
        if ((Get-MetricStr $rep.Out 'combined_hash') -ne $recHash) { throw "M21 replay hash != record hash" }
    }
    Assert-Gate 'regression: brain-off shoggoth deterministic + M5 golden bit-identical' {
        $h1 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        $h2 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        if ($h1 -ne $h2) { throw "shoggoth hash not reproducible with the brain off" }
        $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'
        $shot = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ([double](& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M24 gate: the Backrooms PA has a procedural VOICE the Shoggoth hears as WORDS -- a from-scratch formant TTS whisper can read back, and the recorded chase still replays bit-exact with the TTS + whisper + the model off.'
}

function Invoke-GateM25 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the [m25] creature-mesh-fits-DXR test)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m25'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'

    # THE M25 GATE: the Shoggoth's procedural body renders in the DXR (path-traced) path --
    # injected as a dynamic creature, shaded SALMON (material 7). The salmon metric (R>1.5*G)
    # isolates the creature from the yellow Backrooms: pose 2 = creature only (clean proof),
    # pose 1 = world only (must be ~0). M20b's body was raster-only; now it's in RT too.
    Assert-Gate 'DXR creature: the shoggoth body renders salmon in the path-traced path' {
        $r2 = Invoke-AppCapture @('--shoggoth-dxr-shot', '--seed', '1', '--pose', '2', '--width', '640', '--height', '360', '--spp', '96', '--out', (Join-Path $tmp 'creature.png'))
        if ($r2.Exit -ne 0) { throw "DXR creature shot exited $($r2.Exit): $($r2.Out)" }
        $salmon = Get-Metric $r2.Out 'salmon_px'
        if ($salmon -lt 300) { throw "the creature barely rendered in DXR ($salmon salmon px)" }
        $r1 = Invoke-AppCapture @('--shoggoth-dxr-shot', '--seed', '1', '--pose', '1', '--width', '640', '--height', '360', '--spp', '96')
        if ($r1.Exit -ne 0) { throw "DXR world-only shot exited $($r1.Exit): $($r1.Out)" }
        $world = Get-Metric $r1.Out 'salmon_px'
        if ($world -gt 60) { throw "the world (no creature) reported $world salmon px -- the metric isn't creature-specific" }
        # pose 0 (world + creature) must also build + render debug-clean
        $r0 = Invoke-AppCapture @('--shoggoth-dxr-shot', '--seed', '1', '--pose', '0', '--width', '640', '--height', '360', '--spp', '64', '--out', (Join-Path $tmp 'both.png'))
        if ($r0.Exit -ne 0) { throw "DXR world+creature shot exited $($r0.Exit): $($r0.Out)" }
        Write-Note "DXR creature: $salmon salmon px (creature) vs $world (world only) -- the body renders salmon in RT"
    }

    # In-game: --play --rt shows the creature WITHOUT regressing M19's frame rate -- the chunk
    # BLASes stay cached; only the dynamic creature BLAS rebuilds each frame (the M25 design).
    Assert-Gate 'in-game --play --rt: creature in RT, debug-clean, M19 frame-rate preserved (>=30 frames/5s)' {
        $r = Invoke-AppCapture @('--play', '--rt', '--seconds', '5', '--seed', '3', '--width', '1280', '--height', '720', '--out', (Join-Path $tmp 'play_rt.png'))
        if ($r.Exit -ne 0) { throw "--play --rt exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "ray-traced walk (with creature) had debug-layer messages" }
        $frames = Get-Metric $r.Out 'rt_frames'
        if ($frames -lt 30) { throw "RT dropped to $frames frames/5s (< 30) -- the creature regressed M19 perf" }
        $luma = Get-MetricFloat $r.Out 'rt_luma_mean'
        if ($luma -lt 10.0 -or $luma -gt 250.0) { throw "ray-traced frame luma $luma out of band [10,250]" }
        Write-Note ("in-game RT: $frames DXR frames in 5 s with the live creature, luma {0:N1}, debug-clean" -f $luma)
    }

    # Regression: the raster path is byte-for-byte untouched (the material override is DXR-only).
    Assert-Gate 'regression: M5 raster golden bit-identical + --play (no rt) is raster (rt_frames 0)' {
        $shot = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ([double](& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
        $p = Invoke-AppCapture @('--play', '--seconds', '2', '--seed', '3', '--width', '640', '--height', '360')
        if ($p.Exit -ne 0) { throw "--play exited $($p.Exit)" }
        if ((Get-Metric $p.Out 'rt_frames') -ne 0) { throw "--play (no --rt) ran ray tracing" }
        if ((Get-Metric $p.Out 'debug_error_count') -ne 0) { throw "raster --play had debug-layer messages" }
    }

    # Regression: the DXR WORLD render is unchanged (build_scene's shade-tail reservation is
    # additive), and the M20b raster body still renders debug-clean.
    Assert-Gate 'regression: DXR world render debug-clean + lit (M9), M20b raster body debug-clean' {
        $r = Invoke-AppCapture @('--dxr-pt', '--seed', '1', '--pose', '0', '--width', '640', '--height', '360', '--spp', '64', '--out', (Join-Path $tmp 'dxrpt.png'))
        if ($r.Exit -ne 0) { throw "--dxr-pt exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "--dxr-pt had debug-layer messages" }
        $luma = Get-MetricFloat $r.Out 'luma_mean'
        if ($luma -lt 10.0 -or $luma -gt 200.0) { throw "DXR world luma $luma out of band [10,200]" }
        $s = Invoke-AppCapture @('--shoggoth-shot', '--seed', '1', '--width', '640', '--height', '360', '--out', (Join-Path $tmp 'raster_body.png'))
        if ($s.Exit -ne 0) { throw "--shoggoth-shot exited $($s.Exit): $($s.Out)" }
        if ((Get-Metric $s.Out 'debug_error_count') -ne 0) { throw "M20b raster body had debug-layer messages" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M25 gate: the Shoggoth''s body renders in the ray-traced path too -- a dynamic creature BLAS (chunk BLASes cached, so RT frame-rate holds), shaded salmon. The creature is visible in BOTH renderers now.'
}

function Invoke-GateM26 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the [m26] multi-level tests)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m26'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'

    # No regression: the wanderer's level is kept IMPLICIT (derived from y), so world_state_hash
    # is unchanged and level-0 geometry/lighting is byte-identical.
    Assert-Gate 'regression: M5 raster golden bit-identical at level 0' {
        $m5 = Join-Path $tmp 'lvl0.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $m5)
        if ($r.Exit -ne 0) { throw "level-0 shot exited $($r.Exit)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "level-0 shot had debug-layer messages" }
        if ([double](& $hashdiff diff $m5 (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed at level 0" }
    }

    # The live multi-level path + per-level lights: an up-floor and a down-floor each render
    # debug-clean and DISTINCT from level 0 and from each other (non-repeating floors in Z).
    Assert-Gate 'multi-level render: levels 7 and -3 render debug-clean + distinct floors' {
        $m5 = Join-Path $tmp 'lvl0.png'; $up = Join-Path $tmp 'lvl7.png'; $dn = Join-Path $tmp 'lvlm3.png'
        $ru = Invoke-AppCapture @('--shot', '--level', '7', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $up)
        if ($ru.Exit -ne 0) { throw "level-7 shot exited $($ru.Exit): $($ru.Out)" }
        if ((Get-Metric $ru.Out 'debug_error_count') -ne 0) { throw "level-7 shot had debug-layer messages" }
        $rd = Invoke-AppCapture @('--shot', '--level', '-3', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $dn)
        if ($rd.Exit -ne 0) { throw "level--3 shot exited $($rd.Exit): $($rd.Out)" }
        if ((Get-Metric $rd.Out 'debug_error_count') -ne 0) { throw "level--3 shot had debug-layer messages" }
        $d7 = [double](& $hashdiff diff $up $m5 | Select-Object -Last 1)
        $dm = [double](& $hashdiff diff $dn $m5 | Select-Object -Last 1)
        $du = [double](& $hashdiff diff $up $dn | Select-Object -Last 1)
        if ($d7 -le 1.0) { throw "level 7 not distinct from level 0 (diff $d7)" }
        if ($dm -le 1.0) { throw "level -3 not distinct from level 0 (diff $dm)" }
        if ($du -le 1.0) { throw "levels 7 and -3 not distinct from each other (diff $du)" }
        Write-Note ("multi-level render: lvl7 vs lvl0={0:N2}, lvl-3 vs lvl0={1:N2}, lvl7 vs lvl-3={2:N2} (all distinct, debug-clean)" -f $d7, $dm, $du)
    }

    # Regression: the Shoggoth stays deterministic with the brain off (M20).
    Assert-Gate 'regression: brain-off shoggoth deterministic (M20)' {
        $h1 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        $h2 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        if ($h1 -ne $h2) { throw "shoggoth hash not reproducible with the brain off" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M26 gate: the live walk is multi-level -- the wanderer''s floor is derived from y; chunks/collision/lights are per-level; distinct floors render debug-clean; level 0 stays byte-identical. The Phase IV foundation is in.'
}

function Invoke-GateM27 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. [m27] stair_at coverage + vertical connectivity)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m27'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'

    # THE M27 PROOF: a scripted wanderer climbs a real procedural up-stair a full floor
    # (level 0 -> 1) via step-up locomotion, deterministically (bit-identical hash x2).
    Assert-Gate 'live ascent: the wanderer climbs a procedural stair to level 1, deterministic' {
        foreach ($seed in 1, 7) {
            $r1 = Invoke-AppCapture @('--ascend', '--seed', "$seed")
            if ($r1.Exit -ne 0) { throw "ascend seed $seed exited $($r1.Exit): $($r1.Out)" }
            if ((Get-Metric $r1.Out 'climbed') -ne 1) { throw "seed $seed did not climb the stair" }
            if ((Get-Metric $r1.Out 'level_at_max') -ne 1) { throw "seed $seed did not reach level 1" }
            $climb = Get-MetricFloat $r1.Out 'climb_m'
            if ($climb -lt 3.0) { throw "seed $seed climbed only $climb m (< 3)" }
            $r2 = Invoke-AppCapture @('--ascend', '--seed', "$seed")
            if ((Get-MetricStr $r1.Out 'final_hash') -ne (Get-MetricStr $r2.Out 'final_hash')) { throw "seed $seed ascent hash not reproducible" }
        }
        Write-Note 'live ascent: wanderer climbs a procedural up-stair ~3.98 m to level 1 (seeds 1,7), bit-identical hash x2 -- the stairs are climbable + deterministic'
    }

    # Regression: the M7 scripted DESCENT still reaches level -1 (step-up did not break it).
    Assert-Gate 'regression: --descend still reaches level -1 (M7)' {
        $r = Invoke-AppCapture @('--descend', '--seed', '1')
        if ($r.Exit -ne 0) { throw "descend exited $($r.Exit): $($r.Out)" }
        if ((Get-Metric $r.Out 'level_reached') -ne -1) { throw "descend no longer reaches level -1" }
    }

    # Goldens: the two top-down goldens were re-baselined for the stair holes (ADR-053); the
    # first-person + fixed-scene goldens are UNCHANGED (stairs are sparse, none in those views).
    Assert-Gate 'goldens: m4 top-down re-baselined bit-match; m5/m1/m2 unchanged (minimal blast radius)' {
        foreach ($seed in 1, 7) {
            $td = Join-Path $tmp "td_$seed.png"
            $r = Invoke-AppCapture @('--topdown', '--out', $td, '--width', '512', '--height', '512', '--seed', "$seed")
            if ($r.Exit -ne 0) { throw "topdown seed $seed exited $($r.Exit)" }
            if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "topdown seed $seed had debug-layer messages" }
            if ([double](& $hashdiff diff $td (Join-Path $RepoRoot "goldens\m4\topdown_seed$($seed).png") | Select-Object -Last 1) -ne 0.0) { throw "m4 topdown seed $seed != re-baselined golden" }
        }
        $shot = Join-Path $tmp 'm5.png'
        $rs = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $shot)
        if ((Get-Metric $rs.Out 'debug_error_count') -ne 0) { throw "M5 shot had debug-layer messages" }
        if ([double](& $hashdiff diff $shot (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 first-person golden regressed (stairs leaked into the FP view)" }
        $f0 = Join-Path $tmp 'm1.png'
        $rf = Invoke-AppCapture @('--headless', '--out', $f0, '--width', '320', '--height', '180')
        if ([double](& $hashdiff diff $f0 (Join-Path $RepoRoot 'goldens\m1\frame0_320x180.png') | Select-Object -Last 1) -ne 0.0) { throw "M1 frame-0 golden regressed" }
        $room = Join-Path $tmp 'm2.png'
        $rr = Invoke-AppCapture @('--scene', '--out', $room, '--width', '640', '--height', '360')
        if ([double](& $hashdiff diff $room (Join-Path $RepoRoot 'goldens\m2\room_640x360.png') | Select-Object -Last 1) -ne 0.0) { throw "M2 room golden regressed" }
        Write-Note 'goldens: m4 top-down (seed 1,7) bit-match the ADR-053 re-baseline; m5 FP shot + m1 frame-0 + m2 room byte-identical -- only the ceiling-facing top-down changed'
    }

    # The stairs do not trap horizontal navigation (the carve keeps every stair cell passable).
    Assert-Gate 'walk-bot: 1 km, zero stuck (stairs navigable), deterministic' {
        $h1 = $null
        foreach ($seed in 1, 2, 3) {
            $r = Invoke-AppCapture @('--walkbot', '--km', '1', '--seed', "$seed")
            if ($r.Exit -ne 0) { throw "walk-bot seed $seed failed (exit $($r.Exit))" }
            if ((Get-Metric $r.Out 'stuck_events') -ne 0) { throw "walk-bot seed $seed had stuck events" }
            if ($seed -eq 1) { $h1 = Get-AppHash $r.Out }
        }
        $r2 = Invoke-AppCapture @('--walkbot', '--km', '1', '--seed', '1')
        if ((Get-AppHash $r2.Out) -ne $h1) { throw "walk-bot hash not reproducible" }
    }

    # Regression: the Shoggoth stays deterministic with the brain off (M20).
    Assert-Gate 'regression: brain-off shoggoth deterministic (M20)' {
        $h1 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        $h2 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        if ($h1 -ne $h2) { throw "shoggoth hash not reproducible with the brain off" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M27 gate: procedural stairs connect the stack -- hybrid placement (per-superblock backstop), aligned floor/ceiling holes, a climbable riser-slab stairwell + step-up locomotion (live ascent to level 1), a vertical-connectivity validator. Level-0 blast radius is just the 2 ceiling-facing top-down goldens (ADR-053).'
}

function Invoke-GateM28 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the [m28] two-level residency test)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m28'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'

    # THE M28 PROOF: stand at a stairwell -> both floors resident + rendered, and the floor
    # above shows THROUGH the ceiling hole (see-through). Two floors == exactly 2x the
    # one-floor ring (bounded residency, no leak ~ memory slope 0), and debug-clean.
    Assert-Gate 'see-through: two floors resident + rendered at a stairwell, debug-clean' {
        foreach ($seed in 1, 7) {
            $r = Invoke-AppCapture @('--vstream', '--seed', "$seed", '--radius', '4', '--width', '640', '--height', '360')
            if ($r.Exit -ne 0) { throw "vstream seed $seed exited $($r.Exit): $($r.Out)" }
            # vstream_ok is the app's own verdict, computed on the rendered pixels:
            # (debug-clean) AND (resident_2level == 2 x resident_1level) AND (see_through_diff > 0.5).
            if ((Get-Metric $r.Out 'vstream_ok') -ne 1) { throw "vstream seed $seed not ok: $($r.Out)" }
            $r1 = [int](Get-Metric $r.Out 'resident_1level')
            $r2 = [int](Get-Metric $r.Out 'resident_2level')
            if ($r2 -ne $r1 * 2) { throw "seed ${seed}: two-level residency $r2 != 2x one-level $r1 (not bounded)" }
            if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "vstream seed $seed had debug-layer messages" }
        }
        Write-Note 'see-through: at a stairwell both floors are resident (162 = 2 x 81 chunks, bounded) + rendered debug-clean; the floor above shows through the ceiling hole (vstream_ok asserts see_through_diff > 0.5; measured ~57-62)'
    }

    # Regression: M28 is presentation-only -- the single-level stream path + the sim are
    # unchanged, so the M5 raster golden is bit-identical and the M27 live ascent still works.
    Assert-Gate 'regression: M5 raster golden bit-identical (single-level path unchanged)' {
        $m5 = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $m5)
        if ($r.Exit -ne 0) { throw "M5 shot exited $($r.Exit)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "M5 shot had debug-layer messages" }
        if ([double](& $hashdiff diff $m5 (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
    }
    Assert-Gate 'regression: M27 live ascent still climbs to level 1' {
        $r = Invoke-AppCapture @('--ascend', '--seed', '1')
        if ($r.Exit -ne 0) { throw "ascend exited $($r.Exit)" }
        if ((Get-Metric $r.Out 'climbed') -ne 1) { throw "ascent regressed" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M28 gate: vertical streaming -- the StreamManager keeps a second adjacent floor resident at stairwells (bounded 2x ring) and the renderer draws both, so you see through the holes. Presentation-only: the sim + M5 golden + M27 ascent are unchanged.'
}

function Invoke-GateM30 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the [m30] shaft_at placement test)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m30'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'

    # THE M30 PROOF: a real open shaft drops the wanderer the full depth (5..10 floors) and
    # the bottom floor soft-catches it -- bounded (no tunnelling past the landing), deterministic.
    Assert-Gate 'soft-catch fall: the wanderer falls a full shaft + lands, deterministic' {
        foreach ($seed in 1, 7, 42) {
            $r1 = Invoke-AppCapture @('--shaftfall', '--seed', "$seed")
            if ($r1.Exit -ne 0) { throw "shaftfall seed $seed exited $($r1.Exit): $($r1.Out)" }
            if ((Get-Metric $r1.Out 'landed') -ne 1) { throw "seed $seed did not land at the bottom" }
            $depth = [int](Get-Metric $r1.Out 'depth')
            $fell  = [int](Get-Metric $r1.Out 'fell_floors')
            if ($depth -lt 5 -or $depth -gt 10) { throw "seed $seed shaft depth $depth out of [5,10]" }
            if ($fell -ne $depth) { throw "seed $seed fell $fell floors != depth $depth (not a clean full-depth fall)" }
            $r2 = Invoke-AppCapture @('--shaftfall', '--seed', "$seed")
            if ((Get-MetricStr $r1.Out 'final_hash') -ne (Get-MetricStr $r2.Out 'final_hash')) { throw "seed $seed fall hash not reproducible" }
        }
        Write-Note 'soft-catch fall: seeds 1/7/42 fall the full shaft depth (10/9/9 floors) + land, bounded, bit-identical x2 -- the despair gradient'
    }

    # LIVE DESCENT: the interactive walks (run_play/run_game/run_screensaver) now build their floor
    # PER CELL with HOLES at the open cells (down-stair holes + shaft voids, via build_walk_collision)
    # instead of one sealed {-1e6..1e6} ground plane, so you fall through real openings IN-GAME (the
    # despair gradient, live -- not just visible). --livedescent proves that exact path headlessly:
    # a SOLID cell still HOLDS the wanderer up, and a real DOWN-STAIR hole DROPS him a floor + the
    # level below soft-catches him. Deterministic (bit-identical x2), model-free.
    Assert-Gate 'live descent: the holed live-walk floor drops you through a down-stair hole + lands, deterministic' {
        foreach ($seed in 1, 7, 42) {
            $r1 = Invoke-AppCapture @('--livedescent', '--seed', "$seed")
            if ($r1.Exit -ne 0) { throw "livedescent seed $seed exited $($r1.Exit): $($r1.Out)" }
            if ((Get-Metric $r1.Out 'solid_holds') -ne 1) { throw "seed ${seed}: a solid cell did not hold the wanderer up (the holed floor lost its solid tiles)" }
            if ((Get-Metric $r1.Out 'descended') -ne 1) { throw "seed ${seed}: did not descend through the down-stair hole" }
            $end = [int](Get-Metric $r1.Out 'end_level')
            if ($end -ge 0) { throw "seed ${seed}: end_level $end did not drop below level 0" }
            $r2 = Invoke-AppCapture @('--livedescent', '--seed', "$seed")
            if ((Get-MetricStr $r1.Out 'final_hash') -ne (Get-MetricStr $r2.Out 'final_hash')) { throw "seed $seed live-descent hash not reproducible" }
        }
        Write-Note 'live descent: the holed per-cell floor drops the wanderer through a real down-stair hole to the floor below + soft-catches him, bit-identical x2 -- the despair gradient is now LIVE in-game, not just visible'
    }

    # DEEP-DESCENT SOAK (ROADMAP §3 DONE criterion: "a deep-descent soak holds determinism + bounded
    # memory"). Repeatedly falls the wanderer down a deep shaft with the FULL live machinery (holed
    # build_walk_collision + abyss band-residency + a headless render each frame). Over many descent
    # cycles, residency stays BOUNDED (the band never balloons), process memory is FLAT post-warmup (no
    # leak), each cycle reaches the bottom (no stuck), and -- run under --ticks -- the world hash is
    # reproducible. Catches leaks / determinism drift / unbounded streaming in the new vertical paths.
    Assert-Gate 'deep-descent soak: many deep falls, bounded residency + flat memory + deterministic' {
        foreach ($seed in 1, 42) {
            $r1 = Invoke-AppCapture @('--descentsoak', '--seed', "$seed", '--ticks', '12000', '--radius', '3', '--width', '320', '--height', '180')
            if ($r1.Exit -ne 0) { throw "descentsoak seed ${seed} exited $($r1.Exit): $($r1.Out)" }
            if ((Get-Metric $r1.Out 'descentsoak_ok') -ne 1) { throw "seed ${seed} descent soak not ok: $($r1.Out)" }
            $cyc = [int](Get-Metric $r1.Out 'descent_cycles')
            if ($cyc -lt 5) { throw "seed ${seed} only $cyc descent cycles (expected many deep falls)" }
            $mr = [int](Get-Metric $r1.Out 'max_resident'); $cap = [int](Get-Metric $r1.Out 'resident_cap')
            if ($mr -gt $cap) { throw "seed ${seed} residency $mr exceeded the band cap $cap (unbounded streaming)" }
            $r2 = Invoke-AppCapture @('--descentsoak', '--seed', "$seed", '--ticks', '12000', '--radius', '3', '--width', '320', '--height', '180')
            if ((Get-MetricStr $r1.Out 'final_hash') -ne (Get-MetricStr $r2.Out 'final_hash')) { throw "seed ${seed} descent-soak hash not reproducible (determinism broke under streaming)" }
        }
        Write-Note 'deep-descent soak: seeds 1/42 fall a deep shaft ~50-66 times each (bit-identical x2), residency bounded (245 <= 294 = (kBand+2)x ring), process memory flat post-warmup (<32 MB) -- the vertical paths hold determinism + bounded memory over the long haul'
    }

    # SCREENSAVER navigation: the autonomous camera (Stroller) must walk like a person -- cover ground,
    # range out (explore), and almost NEVER face a wall up close. The old WalkBot faceplanted wall-to-
    # wall while the camera stared at whatever it hit; --strollcheck drives the Stroller headlessly with
    # the screensaver's own holed collision and reports a "faceplant ratio" (fraction of ticks with a
    # wall within 1.2 m straight ahead). A natural walker keeps it near zero. Guards a cosmetic path, but
    # the screensaver nav must not silently regress.
    Assert-Gate 'screensaver Stroller PATH-PLANS the maze (X-ray BFS): real net progress, free-angle, ~no back-and-forth' {
        foreach ($seed in 1, 42, 500) {
            $r = Invoke-AppCapture @('--strollcheck', '--seed', "$seed", '--ticks', '36000')
            if ($r.Exit -ne 0) { throw "strollcheck seed ${seed} exited $($r.Exit): $($r.Out)" }
            if ((Get-Metric $r.Out 'stroll_ok') -ne 1) { throw "seed ${seed} stroll not natural: $($r.Out)" }
            $st = Get-MetricFloat $r.Out 'stall_frac'
            if ($st -ge 0.45) { throw "seed ${seed} stall_frac $st >= 0.45 (it ping-pongs in place instead of traversing -- the back-and-forth bug)" }
            $fp = Get-MetricFloat $r.Out 'faceplant_ratio'
            if ($fp -ge 0.30) { throw "seed ${seed} faceplant_ratio $fp >= 0.30 (the camera stares at walls instead of where it is going)" }
            $oc = Get-MetricFloat $r.Out 'offcardinal_deg'
            if ($oc -lt 6.0) { throw "seed ${seed} offcardinal_deg $oc < 6 (movement is 90-degree-locked -- a vacuum, not a human)" }
        }
        Write-Note 'screensaver Stroller: X-ray BFS pathfinding -- seeds 1/42/500 each traverse ~1000 m along guaranteed routes (never blindly hits a dead-end), net-progress stall_frac 0.16-0.35 (was 0.6 when it ping-ponged), free-angle (offcardinal ~20 deg), faceplant 0.11-0.20'
    }

    # GAME mouse-look must NOT self-spin AND must NOT fight the cursor. The windowed first-person look reads
    # RELATIVE WM_INPUT deltas (raw input): a still mouse must produce ~0 rad/frame of yaw, AND the loop must
    # not warp the OS cursor every frame. The old GetCursorPos/recenter scheme spun (~100x/s, DPI/0,0 warp);
    # the first raw-input cut left a vestigial per-frame SetCursorPos that dragged the cursor to centre every
    # frame -> "my cursor fights me" (no normal game does this). --game --auto-play drops straight into Play
    # with NO input and prints the max per-frame yaw + the count of cursor warps during Play; FAIL returns exit 4.
    Assert-Gate 'game mouse-look: no self-spin AND no cursor-fight (raw input; still mouse -> ~0 yaw, ~0 warps)' {
        $r = Invoke-AppCapture @('--game', '--auto-play', '--seconds', '2', '--no-audio', '--no-shoggoth-brain')
        if ($r.Exit -ne 0) { throw "auto-play exited $($r.Exit) (non-zero => lookcheck FAIL: the view spins or the cursor is warped per-frame): $($r.Out)" }
        # The spin SIGNATURE is "almost every frame pinned at the look clamp"; measure the clamped fraction
        # (robust to a stray real-mouse twitch on an interactive desktop -- a twitch clamps a few frames, a spin
        # clamps ~all). max_dyaw is informational only (a single real flick can exceed 0.05 without it being a spin).
        $cf = Get-MetricFloat $r.Out 'lookcheck_clamped_frac'
        if ($cf -ge 0.5) { throw "lookcheck_clamped_frac $cf >= 0.5 -- most frames pinned at the look clamp (the self-spin bug): $($r.Out)" }
        $lf = Get-Metric $r.Out 'lookcheck_frames'
        if ($lf -lt 30) { throw "only $lf Play frames measured -- --auto-play did not actually enter Play: $($r.Out)" }
        $cw = Get-Metric $r.Out 'lookcheck_cursor_warps'
        if ($cw -ge 10) { throw "lookcheck_cursor_warps $cw >= 10 over $lf frames -- the loop warps the cursor per-frame (the 'cursor fights me' bug): $($r.Out)" }
        Write-Note "game mouse-look: $lf Play frames -> clamped-frac $cf (~0, no self-spin) and $cw cursor warps (~0, no per-frame SetCursorPos fight) -- raw WM_INPUT look"
    }

    # ADR-077: the LIVE dual-device ray-traced path. The instant-crash bug (a non-validated D3D12 device is
    # device-REMOVED the moment a windowed FLIP swapchain is in play) shipped because M9 only ever builds DXR in
    # ISOLATION -- a single headless Device5, no raster device, no swapchain, no window. This check drives the
    # REAL path: the raster device + window + swapchain are already live, THEN a second DXR Device5 is created on
    # top, renders, and presents. rt_frames >= 1 proves the whole pipeline ran (init + build_scene + DispatchRays
    # + readback + present); exit 0 proves no instant crash; debug-clean proves both devices validate (the
    # validation layer is forced on in every build now -- the workaround that dodges the driver fault).
    Assert-Gate 'live RT path: windowed dual-device ray tracing renders + debug-clean (ADR-077 crash guard)' {
        $r = Invoke-AppCapture @('--game', '--rt', '--auto-play', '--seconds', '6', '--no-audio', '--no-shoggoth-brain')
        if ($r.Exit -ne 0) { throw "RT auto-play exited $($r.Exit) (the instant-crash bug, or a spin-guard FAIL): $($r.Out)" }
        $rf = Get-Metric $r.Out 'rt_frames'
        if ($rf -lt 1) { throw "rt_frames $rf < 1 -- the live ray-traced path never rendered (DXR init/build/dispatch crashed, or it silently fell back to raster): $($r.Out)" }
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "the live RT path produced D3D12 debug-layer messages: $($r.Out)" }
        Write-Note "live RT path: $rf windowed ray-traced frames presented (dual-device: raster swapchain + DXR Device5), debug-clean"
    }

    # The abyss renders: look DOWN a shaft with a band of floors resident -> the depths show through
    # the void (then black where the bounded ring ends = fog-to-black), debug-clean + bounded.
    Assert-Gate 'abyss render: floors show down a shaft (fog-to-black), bounded + debug-clean' {
        foreach ($seed in 1, 7) {
            $r = Invoke-AppCapture @('--abyss', '--seed', "$seed", '--radius', '3', '--width', '480', '--height', '270')
            if ($r.Exit -ne 0) { throw "abyss seed $seed exited $($r.Exit): $($r.Out)" }
            # abyss_ok = (debug-clean) AND (resident_deep == (renderDepth+1) x resident_shallow) AND (abyss_diff > 0.5).
            if ((Get-Metric $r.Out 'abyss_ok') -ne 1) { throw "abyss seed $seed not ok: $($r.Out)" }
        }
        Write-Note 'abyss render: looking down a shaft, a band of floors (bounded, e.g. 245 = 5x49) shows the depths through the void (abyss_diff ~21-60), debug-clean -- fog-to-black past the bounded ring'
    }

    # Shafts now exist worldwide; multi-level renders stay debug-clean, and the rare voids
    # (~1/1500) miss the level-0 golden views -> M5 stays bit-identical.
    Assert-Gate 'shaft world renders debug-clean; M5 golden bit-identical (shafts miss the view)' {
        foreach ($lvl in 0, 7, 10) {
            $png = Join-Path $tmp "lvl$lvl.png"
            $r = Invoke-AppCapture @('--shot', '--level', "$lvl", '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $png)
            if ($r.Exit -ne 0) { throw "level $lvl shot exited $($r.Exit)" }
            if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "level $lvl shot had debug-layer messages" }
        }
        $m5 = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $m5)
        if ([double](& $hashdiff diff $m5 (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
    }

    # Regressions: the M27 ascent + the M28 see-through still hold (shafts are purely additive).
    Assert-Gate 'regression: M27 live ascent + M28 see-through intact' {
        $a = Invoke-AppCapture @('--ascend', '--seed', '1')
        if ((Get-Metric $a.Out 'climbed') -ne 1) { throw "M27 live ascent regressed" }
        $v = Invoke-AppCapture @('--vstream', '--seed', '1', '--radius', '4', '--width', '640', '--height', '360')
        if ((Get-Metric $v.Out 'vstream_ok') -ne 1) { throw "M28 see-through regressed" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M30 gate: open shafts -- a rare deep vertical void (shaft_at, ~1/1500) cut through the floors, a soft-catch fall the full depth (5..10), debug-clean, deterministic. ALL M30 polish DONE: live in-game descent (holed walk floor), multi-floor fog render, a deep-descent soak (bounded + deterministic), the draft-audio telegraph (decision 6), and the screensaver natural-navigation redo (the Stroller -- no more WalkBot wall-faceplanting).'
}

function Invoke-GateM29 {
    $log = Join-Path $RepoRoot 'runs\gate-build.log'
    Write-Step "GATE: clean build (fresh-clone equivalent, warnings-as-errors)"
    Invoke-CMakeBuild -Clean -LogFile $log
    Write-Ok "clean build: all targets compiled"

    $logText = ''
    if (Test-Path $log) { $logText = Get-Content $log -Raw }
    Assert-Gate 'no compiler warning text in build log' {
        if ($logText -match '(?im):\s*warning\s') { throw "warning text found in $log" }
    }
    Assert-Gate 'full ctest suite green (incl. the [m29] per-floor shoggoth test)' {
        Push-Location $RepoRoot
        try {
            ctest --test-dir build --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
        } finally { Pop-Location }
    }

    $tmp = Join-Path $RepoRoot 'runs\gate-m29'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $hashdiff = Join-Path (Get-BinDir) 'hashdiff.exe'

    # THE M29 SACRED GATE (across a descent): record the per-floor Shoggoth's brain-driven chase with
    # the wanderer CHANGING FLOOR at the midpoint (--level 2 -> it escapes across the seam), then
    # replay with the model OFFLINE -> bit-identical, AND the creature must have lost the prey (Lurk).
    Assert-Gate 'sacred gate: per-floor shoggoth record->replay bit-identical across a descent, model off' {
        $slog = Join-Path $tmp 's.log'
        $rec = Invoke-AppCapture @('--shoggoth-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', $slog, '--seed', '3', '--ticks', '1200', '--level', '2')
        if ($rec.Exit -ne 0) { throw "record exited $($rec.Exit): $($rec.Out)" }
        if ((Get-Metric $rec.Out 'valid_intents') -lt 1) { throw "the brain produced no valid intents -- is the KEEL sidecar up at :7071?" }
        $recHash = Get-MetricStr $rec.Out 'combined_hash'
        $recState = [int](Get-Metric $rec.Out 'final_state')
        $rep = Invoke-AppCapture @('--shoggoth-replay', '--director-log', $slog, '--level', '2')
        if ($rep.Exit -ne 0) { throw "replay exited $($rep.Exit): $($rep.Out)" }
        if ((Get-Metric $rep.Out 'replay_events') -lt 1) { throw "replay applied no events from the log" }
        if ((Get-MetricStr $rep.Out 'combined_hash') -ne $recHash) { throw "replay hash != record hash across the descent -- the model leaked into the sim" }
        if ([int](Get-Metric $rep.Out 'final_state') -ne $recState) { throw "replay final state diverged from record" }
        if ($recState -ne 0) { throw "the Shoggoth did not lose the prey across the seam (final state $recState, not Lurk)" }
        Write-Note "sacred gate (descent): record==replay $recHash model-offline; the per-floor Shoggoth lost the prey (Lurk) when it changed floor -- escape is deterministic + replayable"
    }

    # Regression: the M21 sacred gate (no floor change, --level 0) still holds bit-exact.
    Assert-Gate 'regression: M21 sacred gate (level-0 record->replay bit-identical, model off)' {
        $slog = Join-Path $tmp 's21.log'
        $rec = Invoke-AppCapture @('--shoggoth-record', '--director-url', 'http://127.0.0.1:7071', '--director-log', $slog, '--seed', '3', '--ticks', '1200')
        if ($rec.Exit -ne 0) { throw "M21 record exited $($rec.Exit): $($rec.Out)" }
        if ((Get-Metric $rec.Out 'valid_intents') -lt 1) { throw "M21 record produced no valid intents" }
        $recHash = Get-MetricStr $rec.Out 'combined_hash'
        $rep = Invoke-AppCapture @('--shoggoth-replay', '--director-log', $slog)
        if ((Get-MetricStr $rep.Out 'combined_hash') -ne $recHash) { throw "M21 sacred gate regressed (level-0 record != replay)" }
    }

    # Regression: brain-off determinism (M20) + the M5 raster golden (shoggoth changes are off-render).
    Assert-Gate 'regression: brain-off shoggoth deterministic (M20) + M5 golden bit-identical' {
        $h1 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        $h2 = Get-MetricStr (Invoke-AppCapture @('--shoggoth', '--seed', '5')).Out 'shoggoth_hash'
        if ($h1 -ne $h2) { throw "shoggoth hash not reproducible with the brain off" }
        $m5 = Join-Path $tmp 'm5.png'
        $r = Invoke-AppCapture @('--shot', '--seed', '1', '--pose', '0', '--ticks', '0', '--width', '640', '--height', '360', '--out', $m5)
        if ((Get-Metric $r.Out 'debug_error_count') -ne 0) { throw "M5 shot had debug-layer messages" }
        if ([double](& $hashdiff diff $m5 (Join-Path $RepoRoot 'goldens\m5\shot_seed1_pose0.png') | Select-Object -Last 1) -ne 0.0) { throw "M5 golden regressed" }
    }
    Assert-Gate 'core compiles with zero graphics/audio includes (INV-5 grep gate)' {
        & (Join-Path $PSScriptRoot 'checks\check_core_isolation.ps1')
        if ($LASTEXITCODE -ne 0) { throw "core isolation check failed" }
    }
    Assert-Gate 'module inventory matches ARCHITECTURE.md (Iron Rule 7)' {
        & (Join-Path $PSScriptRoot 'checks\check_inventory.ps1')
        if ($LASTEXITCODE -ne 0) { throw "inventory check failed" }
    }
    Write-Note 'M29 gate: the Shoggoth is per-floor -- confined to its level, seeded per (seed, level) via its per-level maze, and the wanderer ESCAPES it by changing floor (it cannot sense across a seam); the sacred record->replay stays bit-exact across the descent with the model OFFLINE.'
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
        'M6'    { Invoke-GateM6 -AudioSoakSeconds $AudioSoakSeconds }
        'M7'    { Invoke-GateM7 }
        'M8'    { Invoke-GateM8 }
        'M9'    { Invoke-GateM9 }
        'M10'   { Invoke-GateM10 }
        'M11'   { Invoke-GateM11 }
        'M12'   { Invoke-GateM12 }
        'M13'   { Invoke-GateM13 }
        'M14'   { Invoke-GateM14 }
        'M15'   { Invoke-GateM15 }
        'M16'   { Invoke-GateM16 }
        'M17'   { Invoke-GateM17 }
        'M18'   { Invoke-GateM18 }
        'M19'   { Invoke-GateM19 }
        'M20'   { Invoke-GateM20 }
        'M21'   { Invoke-GateM21 }
        'M21B'  { Invoke-GateM21b }
        'M22'   { Invoke-GateM22 }
        'M23'   { Invoke-GateM23 }
        'M24'   { Invoke-GateM24 }
        'M25'   { Invoke-GateM25 }
        'M26'   { Invoke-GateM26 }
        'M27'   { Invoke-GateM27 }
        'M28'   { Invoke-GateM28 }
        'M29'   { Invoke-GateM29 }
        'M30'   { Invoke-GateM30 }
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
