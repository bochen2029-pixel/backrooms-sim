# package.ps1 — assemble the SELF-CONTAINED portable Backrooms bundle (ADR-076).
#
#   scripts/package.ps1               # build release -> stage dist\Backrooms\ -> zip (store mode)
#   scripts/package.ps1 -StageOnly    # stage the folder only (skip the zip) -- for test-before-zip
#   scripts/package.ps1 -SkipBuild    # reuse the existing build-release exe
#
# The bundle ships the exe at the ROOT with runtime\{llama,keel,whisper}\ + models\ beside it; the exe
# resolves everything exe-relative (ADR-076), so it NEVER looks at C:\models / C:\llama.cpp / C:\whisper.cpp /
# C:\keel-sidecar-7071. Copy this folder anywhere (or zip / butler-push it) and it plays plug-and-play on a
# Win10/11 + RTX box. The source installs still live at C:\... on the dev box; this only COPIES from them.
[CmdletBinding()]
param([switch]$StageOnly, [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"
Ensure-Vcpkg
Enter-VsDevEnv

# --- source installs (dev box) -------------------------------------------------
$LLAMA   = 'C:\llama.cpp'
$WHISPER = 'C:\whisper.cpp'
$KEEL    = 'C:\keel-sidecar-7071'
$MODELS  = 'C:\models'
# the ONLY model files the bundle needs (9B+mmproj vision tier, 4B text tier, whisper base.en for voice).
$MODEL_FILES = @('Qwen3.5-9B-Q5_K_M.gguf', 'mmproj-F16.gguf', 'Qwen3.5-4B-Q4_K_M.gguf', 'ggml-base.en.bin')

$rel   = Join-Path $RepoRoot 'build-release'
$dist  = Join-Path $RepoRoot 'dist'
$stage = Join-Path $dist 'Backrooms'
$zip   = Join-Path $dist 'Backrooms-portable.zip'

function Find-SdkDll([string]$name) {
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "$env:ProgramFiles\Windows Kits\10\bin")
    $hits = @()
    foreach ($r in $roots) {
        if (Test-Path $r) { $hits += Get-ChildItem -Path $r -Recurse -Filter $name -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match '\\x64\\' } }
    }
    if (-not $hits) { throw "could not find $name in the Windows SDK (needed to bundle the DXR compiler)" }
    ($hits | Sort-Object FullName -Descending | Select-Object -First 1).FullName
}
function Need([string]$p) { if (-not (Test-Path $p)) { throw "missing source: $p" }; $p }

# --- 1) build release ----------------------------------------------------------
if (-not $SkipBuild) {
    Write-Step "build release (BACKROOMS_RELEASE=ON, optimized; ADR-077: validation layer still attempted -- no-op without the SDK layers)"
    $toolchain = Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'
    cmake -S $RepoRoot -B $rel -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DVCPKG_TARGET_TRIPLET=x64-windows-static" "-DVCPKG_HOST_TRIPLET=x64-windows-static" `
        "-DCMAKE_BUILD_TYPE=Release" "-DBACKROOMS_RELEASE=ON"
    if ($LASTEXITCODE -ne 0) { throw "release configure failed" }
    cmake --build $rel --target backrooms
    if ($LASTEXITCODE -ne 0) { throw "release build failed" }
}
$exe = Need (Join-Path $rel 'bin\backrooms.exe')

# --- 2) clean stage tree -------------------------------------------------------
Write-Step "stage -> $stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
foreach ($d in @('', 'runtime\llama', 'runtime\keel', 'runtime\whisper', 'models', 'licenses', 'logs')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $stage $d) | Out-Null
}

# exe at the bundle ROOT (the exe-relative resolver finds runtime\ + models\ beside it).
Copy-Item $exe (Join-Path $stage 'Backrooms.exe')

# DXR shader compiler (redistributable) next to the exe, so DXR works with no Windows SDK installed.
Copy-Item (Find-SdkDll 'dxcompiler.dll') (Join-Path $stage 'dxcompiler.dll')
Copy-Item (Find-SdkDll 'dxil.dll')       (Join-Path $stage 'dxil.dll')
Write-Note "bundled DXC (dxcompiler.dll + dxil.dll)"

# --- 3) llama runtime: the server stub + ALL its DLLs (guaranteed-complete; the spare ggml-cpu-* are ~1 MB each) ---
Write-Step "copy llama runtime (~1.1 GB of CUDA DLLs)"
Copy-Item (Need (Join-Path $LLAMA 'llama-server.exe')) (Join-Path $stage 'runtime\llama')
Get-ChildItem (Join-Path $LLAMA '*.dll') | Copy-Item -Destination (Join-Path $stage 'runtime\llama')

# --- 4) keel sidecar: the binary + its lock (probe-and-reuse :8080; the C:\ paths in the lock stay vestigial) ---
Copy-Item (Need (Join-Path $KEEL 'keel-serve.exe')) (Join-Path $stage 'runtime\keel')
Copy-Item (Need (Join-Path $KEEL 'keel.lock'))      (Join-Path $stage 'runtime\keel')

# --- 5) whisper runtime: the CLI + its DLLs (CPU; the user's speech transcription) ---
Copy-Item (Need (Join-Path $WHISPER 'whisper-cli.exe')) (Join-Path $stage 'runtime\whisper')
Get-ChildItem (Join-Path $WHISPER '*.dll') | Copy-Item -Destination (Join-Path $stage 'runtime\whisper')

# --- 6) models (only the needed ones) ------------------------------------------
Write-Step "copy models (~9.9 GB) -- this is the slow part"
foreach ($m in $MODEL_FILES) { Copy-Item (Need (Join-Path $MODELS $m)) (Join-Path $stage 'models') }

# --- 7) credits / licenses / readme / launcher --------------------------------
& $exe --credits | Out-File -Encoding ASCII (Join-Path $stage 'CREDITS.txt')
@"
BACKROOMS SIM -- third-party components bundled in this portable build
=====================================================================
This game bundles local AI models + inference runtimes so it runs offline, plug-and-play.

  Qwen3.5 (9B-Q5, 4B-Q4, mmproj-F16)  -- Alibaba/Qwen. Apache-2.0 (verify the GGUF's model-card tag).
  llama.cpp + ggml (runtime\llama)    -- MIT (ggerganov/llama.cpp).
  whisper.cpp + model (runtime\whisper, models\ggml-base.en.bin) -- MIT (ggerganov/whisper.cpp; OpenAI Whisper MIT).
  NVIDIA CUDA runtime DLLs (cudart/cublas/cublasLt, in runtime\llama) -- redistributable per the CUDA EULA
     (embedded-in-application redistribution). The driver's nvcuda.dll is NOT bundled (it's the user's driver).
  KEEL sidecar (runtime\keel\keel-serve.exe) -- the project's own component.
  DirectX Shader Compiler (dxcompiler.dll, dxil.dll) -- Microsoft, redistributable (MIT-licensed DXC).

Everything else (the world, textures, audio, the PA voice) is generated procedurally -- no asset files.
Before public distribution, confirm each component's license text is included as required by its terms.
"@ | Out-File -Encoding ASCII (Join-Path $stage 'licenses\NOTICE.txt')

@"
BACKROOMS SIM (portable, self-contained)
========================================
Double-click Backrooms.exe to play. Everything it needs is in this folder -- no install, no setup.

Controls:  WASD / arrows move | Mouse look | Space jump | Shift run | Esc pause | F11 fullscreen | F2 ray tracing
Talk to it:  with DIRECTOR on, the facility intelligence SEES your view and narrates it -- and you can SPEAK
             into your mic and it answers, aloud + subtitled. Try Settings -> TEST MICROPHONE first.

Requirements:  Windows 10/11 x64. A Direct3D-12 GPU. For the local AI Director/voice: an NVIDIA RTX GPU +
               a recent driver (>= 551.61). The AI auto-selects by VRAM (>= 12 GB -> the 9B model with vision;
               less -> the 4B model). With no/old GPU the game still plays -- the AI simply stays quiet.
               First launch spends ~20-40 s loading the model in the background before the Director speaks.

No data leaves your machine -- all inference is local. See CREDITS.txt and licenses\NOTICE.txt.
"@ | Out-File -Encoding ASCII (Join-Path $stage 'README.txt')

@'
@echo off
start "" "%~dp0Backrooms.exe"
'@ | Out-File -Encoding ASCII (Join-Path $stage 'RUN.cmd')

# --- 8) report sizes -----------------------------------------------------------
$total = (Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum
Write-Ok ("staged -> {0}  ({1:N1} GB, {2} files)" -f $stage, ($total / 1GB), (Get-ChildItem $stage -Recurse -File).Count)
if ($StageOnly) { exit 0 }

# --- 9) zip (STORE mode: the GGUFs are incompressible, so don't waste time deflating) ---
Write-Step "zip (store mode) -> $zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path $zip) { Remove-Item -Force $zip }
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::NoCompression, $true)
Write-Ok ("portable zip -> {0}  ({1:N1} GB)" -f $zip, ((Get-Item $zip).Length / 1GB))
Write-Note "for itch.io > 2 GB, prefer 'butler push dist\Backrooms <you>/backrooms:windows' (folder, cheap patches) over the web uploader."
