[CmdletBinding()]
param(
    [string]$BuildDir = 'build\win-native',
    [string]$OutputDir = 'build\setup-dev',
    [int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 1)
)

# Everyday loop: rebuild everything from the checkout and restage build\setup-dev, then run its launcher in
# development mode (no update check, no download, nothing published).
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -BuildDir $BuildDir -OutputDir $OutputDir -Jobs $Jobs
Write-Host 'TriRod local development rebuild finished.'
