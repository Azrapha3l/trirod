[CmdletBinding()]
param(
    [string]$BuildDir = 'build\win-native',
    [string]$OutputDir = 'build\setup-dev',
    [int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 1),
    [switch]$FnProf
)

# Everyday loop: rebuild everything from the checkout and restage build\setup-dev, then run its launcher in
# development mode (no update check, no download, nothing published).
$ErrorActionPreference = 'Stop'
$buildParams = @{ BuildDir = $BuildDir; OutputDir = $OutputDir; Jobs = $Jobs }
if ($FnProf) { $buildParams.FnProf = $true }
& (Join-Path $PSScriptRoot 'build.ps1') @buildParams
Write-Host 'TriRod local development rebuild finished.'
