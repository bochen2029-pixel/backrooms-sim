# package.ps1 — M17: build the packaged release + a portable .zip (no installer).
#
#   scripts/package.ps1               # build release -> dist/backrooms-portable.zip
#   scripts/package.ps1 -StageOnly    # stage the folder, skip the zip
#
# The release config (BACKROOMS_RELEASE=ON) compiles the D3D12 debug layer/DRED out,
# optimizes, and the portable folder bundles dxcompiler.dll/dxil.dll so an end user
# needs no Windows SDK. Everything else is procedural — no asset files.
[CmdletBinding()]
param([switch]$StageOnly)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"
Ensure-Vcpkg
Enter-VsDevEnv

# Locate a DLL under the Windows SDK x64 bin (newest version wins).
function Find-SdkDll([string]$name) {
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "$env:ProgramFiles\Windows Kits\10\bin")
    $hits = @()
    foreach ($r in $roots) {
        if (Test-Path $r) {
            $hits += Get-ChildItem -Path $r -Recurse -Filter $name -ErrorAction SilentlyContinue |
                     Where-Object { $_.FullName -match '\\x64\\' }
        }
    }
    if (-not $hits) { throw "could not find $name in the Windows SDK (needed to bundle the DXR compiler)" }
    ($hits | Sort-Object FullName -Descending | Select-Object -First 1).FullName
}

$rel   = Join-Path $RepoRoot 'build-release'
$dist  = Join-Path $RepoRoot 'dist'
$stage = Join-Path $dist 'backrooms'
$zip   = Join-Path $dist 'backrooms-portable.zip'
$toolchain = Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'

Write-Step "configure + build release (BACKROOMS_RELEASE=ON, optimized, no debug layer)"
cmake -S $RepoRoot -B $rel -G Ninja `
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
  "-DVCPKG_TARGET_TRIPLET=x64-windows-static" "-DVCPKG_HOST_TRIPLET=x64-windows-static" `
  "-DCMAKE_BUILD_TYPE=Release" "-DBACKROOMS_RELEASE=ON"
if ($LASTEXITCODE -ne 0) { throw "release configure failed" }
cmake --build $rel --target backrooms
if ($LASTEXITCODE -ne 0) { throw "release build failed" }

$exe = Join-Path $rel 'bin\backrooms.exe'
if (-not (Test-Path $exe)) { throw "release exe not built: $exe" }

Write-Step "stage portable folder"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $exe (Join-Path $stage 'backrooms.exe')

# Bundle the DXC compiler + signer so end users need no Windows SDK (the DXR path).
$dxc  = Find-SdkDll 'dxcompiler.dll'
$dxil = Find-SdkDll 'dxil.dll'
Copy-Item $dxc  (Join-Path $stage 'dxcompiler.dll')
Copy-Item $dxil (Join-Path $stage 'dxil.dll')
Write-Note "bundled DXC: $(Split-Path (Split-Path $dxc -Parent) -Leaf)\dxcompiler.dll + dxil.dll"

# CREDITS.txt is the app's own --credits output (single source of truth).
& $exe --credits | Out-File -Encoding ASCII (Join-Path $stage 'CREDITS.txt')

@'
@echo off
start "" "%~dp0backrooms.exe" --game
'@ | Out-File -Encoding ASCII (Join-Path $stage 'RUN.cmd')

@"
BACKROOMS SIM (portable)
========================
Double-click RUN.cmd (or run backrooms.exe --game) to play.

Controls
  WASD / arrows  move         Mouse  look          Space  jump
  Esc            pause / back  Enter  select        F11    fullscreen
  Gamepad (XInput): left stick move, right stick look, A jump, Start pause.

Main menu: New Game, Continue, Settings, Quit. Settings (volumes, mouse,
fullscreen, resolution, seed) persist in backrooms.cfg next to the exe.

Other modes: --dxr-pt (path-traced photo), --shot (raster photo),
--render-wav (offline audio), --credits.

Requirements: Windows 10/11 x64, a Direct3D 12 GPU (DXR for --dxr-pt). No
install and no SDK needed -- dxcompiler.dll and dxil.dll are bundled here.
Everything is procedural; there are no asset files.

See CREDITS.txt. Version 2.0.
"@ | Out-File -Encoding ASCII (Join-Path $stage 'README.txt')

if ($StageOnly) { Write-Ok "staged -> $stage"; exit 0 }

Write-Step "zip"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Ok "portable package -> $zip ($size MB)"
