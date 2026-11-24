[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Version,
    [string]$MinLauncher = 'alpha_1',
    [string]$UpdateRepo = 'Azrapha3l/trirod',
    [string]$BuildDir = 'build\win-native',
    [string]$PackageDir = 'build\setup-release',
    [string]$ReleaseDir = 'build\release',
    [int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 1),
    [switch]$Required
)

# One local command for preparing a release. It updates the source version, builds the launcher/tools,
# and creates the zip plus release manifest. It never pushes or uploads anything.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$versionPattern = '^(alpha_[0-9]+|beta_[0-9]+|v?[0-9]+\.[0-9]+\.[0-9]+)$'
foreach ($entry in @(@{ Name = 'Version'; Value = $Version }, @{ Name = 'MinLauncher'; Value = $MinLauncher })) {
    if ($entry.Value -notmatch $versionPattern) {
        throw "$($entry.Name) '$($entry.Value)' is invalid. Use alpha_N, beta_N, or vMAJOR.MINOR.PATCH."
    }
}
if ($UpdateRepo -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
    throw "UpdateRepo '$UpdateRepo' must be in owner/repository form."
}

$launcherSource = Join-Path $root 'runtime\ui\launcher.mm'
$source = [IO.File]::ReadAllText($launcherSource, [Text.Encoding]::UTF8)
$versionDefine = '(?m)^(#define EOWRECOMP_VERSION ")[^"]+("\s*)$'
$matches = [regex]::Matches($source, $versionDefine)
if ($matches.Count -ne 1) { throw "Expected exactly one EOWRECOMP_VERSION definition in $launcherSource; found $($matches.Count)." }
$versionRegex = [regex]::new($versionDefine)
$updatedSource = $versionRegex.Replace($source, ('${1}' + $Version + '${2}'), 1)
[IO.File]::WriteAllText($launcherSource, $updatedSource, (New-Object Text.UTF8Encoding($false)))
Write-Host "Set launcher version to $Version in runtime\ui\launcher.mm."

Push-Location $root
try {
    $buildParams = @{
        BuildDir = $BuildDir
        OutputDir = $PackageDir
        Jobs = $Jobs
        UpdateRepo = $UpdateRepo
    }
    & (Join-Path $PSScriptRoot 'build.ps1') @buildParams

    $toolsManifest = Join-Path (Join-Path $root $PackageDir) 'tools\trirod-tools.json'
    if (-not (Test-Path -LiteralPath $toolsManifest -PathType Leaf)) {
        throw "Build completed without producing $toolsManifest."
    }
    $builtVersion = (Get-Content -LiteralPath $toolsManifest -Raw | ConvertFrom-Json).version
    if ($builtVersion -ne $Version) {
        throw "Built package version is '$builtVersion', expected '$Version'. Check the build output and Makefile version extraction."
    }

    $releaseParams = @{
        PackageDir = $PackageDir
        OutputDir = $ReleaseDir
        UpdateRepo = $UpdateRepo
        MinLauncher = $MinLauncher
    }
    if ($Required) { $releaseParams.Required = $true }
    & (Join-Path $PSScriptRoot 'make_release.ps1') @releaseParams

    Write-Host ''
    Write-Host "Local release is ready: $(Join-Path (Join-Path $root $ReleaseDir) $Version)"
    Write-Host 'Upload/publish is not performed by this script.'
}
finally {
    Pop-Location
}
