# common.ps1 — shared helpers for the build/gate scripts.
# Dot-sourced by the other scripts. Windows PowerShell 5.1 compatible
# (no ternary / null-coalescing / && ; these scripts run via powershell.exe).

Set-StrictMode -Version Latest

# Repo root = grandparent of this file (scripts/lib/common.ps1 -> repo).
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Write-Step([string]$m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok([string]$m)   { Write-Host "[ok] $m" -ForegroundColor Green }
function Write-Note([string]$m) { Write-Host "[..] $m" -ForegroundColor DarkGray }
function Write-Fail([string]$m) { Write-Host "[XX] $m" -ForegroundColor Red }

# Run a native command; throw on nonzero exit.
function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$File,
        [string[]]$Arguments = @()
    )
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "command failed (exit $LASTEXITCODE): $File $($Arguments -join ' ')"
    }
}

# Import the MSVC x64 developer environment (compiler, linker, SDK) and ensure
# the VS-bundled Ninja is on PATH (it is not installed globally on this machine).
function Enter-VsDevEnv {
    $pf86 = ${env:ProgramFiles(x86)}
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found; install Visual Studio 2022 with the C++ workload"
    }
    $installPath = (& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath) | Select-Object -First 1
    if (-not $installPath) { throw "no VS install with C++ tools found" }

    # Make the bundled Ninja discoverable.
    $ninjaDir = Join-Path $installPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    if ((Test-Path (Join-Path $ninjaDir 'ninja.exe')) -and ($env:PATH -notlike "*$ninjaDir*")) {
        $env:PATH = "$ninjaDir;$env:PATH"
    }

    # Import the compiler environment once per process.
    if (-not $env:VSCMD_VER) {
        $vcvars = Join-Path $installPath 'VC\Auxiliary\Build\vcvars64.bat'
        if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }
        $cmdline = '"' + $vcvars + '" >nul 2>&1 && set'
        $captured = & $env:ComSpec /c $cmdline
        foreach ($line in $captured) {
            if ($line -match '^([^=]+)=(.*)$') {
                Set-Item -Path "env:$($matches[1])" -Value $matches[2]
            }
        }
        if (-not $env:VSCMD_VER) { throw "failed to import VS dev environment" }
    }
}

# Ensure vcpkg exists; set $env:VCPKG_ROOT. Search order: existing VCPKG_ROOT,
# C:\vcpkg, repo extern/vcpkg. Clone+bootstrap into extern/vcpkg if none found
# (so a fresh clone of this repo self-bootstraps).
function Ensure-Vcpkg {
    $candidates = @()
    if ($env:VCPKG_ROOT) { $candidates += $env:VCPKG_ROOT }
    $candidates += 'C:\vcpkg'
    $candidates += (Join-Path $RepoRoot 'extern\vcpkg')
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c 'vcpkg.exe'))) {
            $env:VCPKG_ROOT = (Resolve-Path $c).Path
            Write-Note "vcpkg: $env:VCPKG_ROOT"
            return
        }
    }
    $target = Join-Path $RepoRoot 'extern\vcpkg'
    Write-Step "vcpkg not found; cloning (shallow) into $target"
    Invoke-Native git @('clone', '--depth', '1', 'https://github.com/microsoft/vcpkg.git', $target)
    Invoke-Native (Join-Path $target 'bootstrap-vcpkg.bat') @('-disableMetrics')
    $env:VCPKG_ROOT = (Resolve-Path $target).Path
}

# Configure (if needed) and build with the ninja-vcpkg preset. Optional -Clean
# wipes the build dir first; optional -LogFile tees all output for scanning.
function Invoke-CMakeBuild {
    param(
        [switch]$Clean,
        [string]$LogFile = ''
    )
    Ensure-Vcpkg
    Enter-VsDevEnv
    $build = Join-Path $RepoRoot 'build'
    if ($Clean -and (Test-Path $build)) {
        Write-Step "clean: removing $build"
        Remove-Item -Recurse -Force $build
    }
    if ($LogFile -and (Test-Path $LogFile)) { Remove-Item -Force $LogFile }

    Push-Location $RepoRoot
    try {
        $needConfigure = $Clean -or (-not (Test-Path (Join-Path $build 'CMakeCache.txt')))
        if ($needConfigure) {
            Write-Step "configure: cmake --preset ninja-vcpkg"
            if ($LogFile) {
                cmake --preset ninja-vcpkg 2>&1 | Tee-Object -FilePath $LogFile -Append
            } else {
                cmake --preset ninja-vcpkg
            }
            if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
        }
        Write-Step "build: cmake --build build"
        if ($LogFile) {
            cmake --build $build 2>&1 | Tee-Object -FilePath $LogFile -Append
        } else {
            cmake --build $build
        }
        if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
    } finally {
        Pop-Location
    }
}

function Get-BinDir { return (Join-Path $RepoRoot 'build\bin') }
