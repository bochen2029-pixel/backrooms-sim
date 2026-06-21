# package.ps1 — assemble the SELF-CONTAINED portable Backrooms bundle (ADR-076).
#
#   scripts/package.ps1               # build release -> stage dist\Backrooms\ -> zip (store mode)
#   scripts/package.ps1 -StageOnly    # stage the folder only (skip the zip) -- for test-before-zip
#   scripts/package.ps1 -SkipBuild    # reuse the existing build-release exe
#
# The bundle ships the exe at the ROOT with runtime\{llama,keel,whisper}\ + models\ beside it; the exe
# resolves everything exe-relative (ADR-076), so it NEVER looks at C:\models / C:\llama.cpp / C:\whisper.cpp /
# C:\keel-sidecar-7071. Copy this folder anywhere (or zip / butler-push it) and it plays plug-and-play on a
# Win10/11 + RTX box. ADR-078: the runtime + models + DXC are now PERSISTENT IN-REPO assets under
# dist\Backrooms; this script refreshes ONLY the exe (which changes per build) + the docs, verifies the
# rest is present, and NEVER sources from C:\ or the Windows SDK. Nothing outside C:\backrooms is needed.
[CmdletBinding()]
param([switch]$StageOnly, [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"
Ensure-Vcpkg
Enter-VsDevEnv

# the model files the bundle needs (9B+mmproj vision tier, 4B text tier, whisper base.en for voice).
# These + the runtime\ binaries + DXC are persistent in-repo assets under dist\Backrooms (ADR-078) --
# never re-sourced from C:\. If a fresh checkout lacks them, restore from C:\backrooms_backups\ or the zip.
$MODEL_FILES = @('Qwen3.5-9B-Q5_K_M.gguf', 'mmproj-F16.gguf', 'Qwen3.5-4B-Q4_K_M.gguf', 'ggml-base.en.bin')

$rel   = Join-Path $RepoRoot 'build-release'
$dist  = Join-Path $RepoRoot 'dist'
$stage = Join-Path $dist 'Backrooms'
$zip   = Join-Path $dist 'Backrooms-portable.zip'

# (Find-SdkDll removed -- ADR-078: dxcompiler.dll/dxil.dll are persistent in-repo assets in dist\Backrooms,
#  no longer fetched from the Windows SDK.)
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

# --- 2) refresh the exe in the IN-REPO bundle; preserve the persistent runtime/models/DXC (ADR-078) ---
# The runtime + models + DXC were staged ONCE (ADR-076) and persist under dist\Backrooms. We do NOT
# wipe them and do NOT re-source from C:\ / the SDK -- only the exe (which changes per build) is refreshed.
Write-Step "refresh exe -> $stage (preserving the in-repo runtime + models + DXC)"
foreach ($d in @('', 'runtime\llama', 'runtime\keel', 'runtime\whisper', 'models', 'licenses', 'logs', 'D3D12')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $stage $d) | Out-Null
}
Copy-Item $exe (Join-Path $stage 'Backrooms.exe') -Force
# Screensaver = the same exe renamed (self-detects /s /p /c). Named "(screensaver)" so a player doesn't mistake it
# for a second game launcher; remove any old plain-named copy from a prior stage.
Remove-Item (Join-Path $stage 'Backrooms.scr') -Force -ErrorAction SilentlyContinue
Copy-Item $exe (Join-Path $stage 'Backrooms (screensaver).scr') -Force
# A prominent "start here" signpost at the root. The leading "!" sorts it to the top of the file list so players
# immediately see which file to run and that the rest is just the engine -- the portable-build findability fix.
@'
================================================================
  BACKROOMS  --  HOW TO PLAY
================================================================

      >>>   Double-click   Backrooms.exe   to play.   <<<

That is the only file you need to run. No install, no setup.

Everything else in this folder (the .dll files and the D3D12 /
models / runtime folders) is the game engine and its built-in
offline AI -- you do not need to open or touch any of it.

Controls:  WASD/arrows move - mouse look - Shift run - Space jump
           Esc pause - F11 fullscreen - F2 ray tracing
           F3 RT quality - V vsync - F flashlight - R flare (in RT)

Slow with ray tracing on? Press V (uncaps the frame rate) and F3
(lowers the ray-tracing resolution). A big speed-up, esp. at 4K.

See README.txt for requirements, the AI/privacy note, and credits.
================================================================
'@ | Out-File -Encoding ASCII (Join-Path $stage '! Double-click Backrooms.exe to play.txt')

# --- 3) verify the persistent in-repo assets are present (NEVER copied from C:\ / SDK) ---
$required = @('dxcompiler.dll', 'dxil.dll',
              'D3D12\D3D12Core.dll', 'D3D12\d3d12SDKLayers.dll',   # ADR-081: Agility SDK redist -> RT works on a clean Win11 (no Graphics Tools)
              'runtime\llama\llama-server.exe', 'runtime\keel\keel-serve.exe', 'runtime\keel\keel.lock',
              'runtime\whisper\whisper-cli.exe') + ($MODEL_FILES | ForEach-Object { "models\$_" })
$missing = @()
foreach ($r in $required) { if (-not (Test-Path (Join-Path $stage $r))) { $missing += $r } }
if ($missing.Count -gt 0) {
    Write-Fail "the in-repo bundle is missing persistent assets (restore from C:\backrooms_backups\ or the zip; package.ps1 no longer sources C:\ / the SDK):"
    $missing | ForEach-Object { Write-Host "    $_" }
    throw "incomplete in-repo runtime under $stage"
}
Write-Ok "in-repo runtime + models + DXC verified present (no external C:\ / Windows SDK source)"

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

# Full third-party license texts (Apache-2.0 for the Qwen models, MIT for the runtimes) -- required for
# public redistribution. Sourced from the committed repo copy so the bundle is always license-complete.
$licSrc = Join-Path $RepoRoot 'licenses\THIRD-PARTY-LICENSES.txt'
if (Test-Path $licSrc) { Copy-Item $licSrc (Join-Path $stage 'licenses\THIRD-PARTY-LICENSES.txt') -Force }
else { Write-Note "licenses\THIRD-PARTY-LICENSES.txt missing -- add it before a public release" }

@"
BACKROOMS SIM (portable, self-contained)
========================================
Double-click Backrooms.exe to play. Everything it needs is in this folder -- no install, no setup.

Controls:  WASD / arrows move | Mouse look | Space jump | Shift run | Esc pause | F11 fullscreen | F2 ray tracing
           F3 RT quality (Quality/Balanced/Performance) | V vsync on/off | F flashlight (RT) | R drop flare (RT)
If ray tracing feels slow:  press V (turns OFF vsync -> uncaps the frame rate) and F3 (lowers the ray-tracing
           resolution -- Balanced or Performance). Together these are a big speed-up, especially at 4K. The image
           stays sharp when you hold still (it refines over a moment); lower RT quality is softest while moving.
Talk to it:  with DIRECTOR on, the facility intelligence SEES your view and narrates it -- and you can SPEAK
             into your mic and it answers, aloud + subtitled. Try Settings -> TEST MICROPHONE first.

Requirements:  Windows 10/11 x64. A Direct3D-12 GPU. For the local AI Director/voice: an NVIDIA RTX GPU +
               a recent driver (>= 551.61). The AI auto-selects by VRAM (>= 12 GB -> the 9B model with vision;
               less -> the 4B model). With no/old GPU the game still plays -- the AI simply stays quiet.
               First launch spends ~20-40 s loading the model in the background before the Director speaks.

No data leaves your machine -- all inference is local. See CREDITS.txt and licenses\NOTICE.txt.
"@ | Out-File -Encoding ASCII (Join-Path $stage 'README.txt')

# (RUN.cmd removed -- ADR-078: the release exe is /SUBSYSTEM:WINDOWS, so users double-click
#  Backrooms.exe directly; the old .cmd launcher flashed a console window, which we no longer want.)
Remove-Item (Join-Path $stage 'RUN.cmd') -Force -ErrorAction SilentlyContinue

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
