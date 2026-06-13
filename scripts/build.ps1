# build.ps1 — one-command build. -Clean wipes the build dir first.
# Bootstraps vcpkg and imports the MSVC dev environment automatically.
[CmdletBinding()]
param([switch]$Clean)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\common.ps1"

try {
    Invoke-CMakeBuild -Clean:$Clean
    Write-Ok "build complete -> $(Get-BinDir)"
    exit 0
} catch {
    Write-Fail $_.Exception.Message
    exit 1
}
