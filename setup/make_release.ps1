[CmdletBinding()]
param(
    # A staged installation from setup/build.ps1 (built with -UpdateRepo so its launcher checks that repository)
    [string]$PackageDir = 'build\setup-dev',
    [string]$OutputDir = 'build\release',
    # owner/name of the GitHub repository the release will be published in: the launcher must have been built for it
    [Parameter(Mandatory = $true)][string]$UpdateRepo,
    # the launcher this release's tools need (default: this release's own version). Informational since the self-update:
    # the package carries that launcher, and installing the update restarts into it.
    [string]$MinLauncher = '',
    # the oldest launcher whose self-update can install this package (default: empty, any launcher that has one). Set it
    # only when the package layout or update.json format changes in a way older self-updates can't apply.
    [string]$MinUpdater = '',
    # recompile even when the build identifiers did not change
    [switch]$Required
)

# Produces the two files a Windows release carries (docs/RELEASE-WORKFLOW.md, "Maintainer release procedure"):
#   TriRod-<version>-Windows-x64.zip   the installation (launcher, trbuild, Trirod-ShaderPrep, runtime DLLs, tools/,
#                                      recomplauncher.json at the top for RecompLauncher)
#   trirod-release.json          what the launcher reads: version, build identifiers, the zip's size and SHA-256
# Nothing is uploaded: attach both files to one GitHub Release yourself. Game files and anything made from them are
# refused (setup/check_package.ps1), and the player's data in a development installation is left out.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pkg = (Resolve-Path -LiteralPath (Join-Path $root $PackageDir)).Path
$toolsJson = Join-Path $pkg 'tools\trirod-tools.json'
if (-not (Test-Path -LiteralPath $toolsJson)) { throw "No tools/trirod-tools.json in ${pkg}: run setup/build.ps1 first." }
$tools = Get-Content -LiteralPath $toolsJson -Raw | ConvertFrom-Json
$version = $tools.version
if (-not $version -or -not $tools.gamelib_id -or -not $tools.gamebuild_id) { throw "$toolsJson has no version / build identifiers." }
if (-not $MinLauncher) { $MinLauncher = $version }

# The launcher carries its update repository as a string (Makefile UPDATE_REPO): a release launcher without it would
# never see the next release.
$launcherText = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes((Join-Path $pkg 'Trirod-Launcher.exe')))
if (-not $launcherText.Contains([char]0 + $UpdateRepo + [char]0)) { throw "Trirod-Launcher.exe was not built with UPDATE_REPO=$UpdateRepo (setup/build.ps1 -UpdateRepo $UpdateRepo)." }

$out = Join-Path $root $OutputDir
$outVersion = Join-Path $out $version
$stage = Join-Path $outVersion 'stage'
if (Test-Path -LiteralPath $outVersion) { Remove-Item -LiteralPath $outVersion -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Only the installation's own files (an allowlist), never the data a development run left beside them
$top = @('Trirod-Launcher.exe', 'trbuild.exe', 'Trirod-ShaderPrep.exe', 'trirod.ico', 'icon.png', 'launcher-skin.png',
         'recomplauncher.json', 'tools')
foreach ($name in $top) {
    $from = Join-Path $pkg $name
    if (-not (Test-Path -LiteralPath $from)) { throw "The package has no $name." }
    Copy-Item -LiteralPath $from -Destination $stage -Recurse
}
Get-ChildItem -LiteralPath $pkg -Filter '*.dll' -File | Where-Object { $_.Name -notin @('Trirod.dll', 'eowgame.dll') } |
    Copy-Item -Destination $stage
# (RecompLauncher skips a recomplauncher.json it can't read, whole: at least the JSON and its format must be right)
$rl = Get-Content -LiteralPath (Join-Path $stage 'recomplauncher.json') -Raw | ConvertFrom-Json
if ($rl.format -ne 1 -or -not $rl.start -or -not $rl.prepare) { throw "recomplauncher.json has no format 1, start or prepare." }
& (Join-Path $PSScriptRoot 'check_package.ps1') -PackageDir $stage

# zip with Windows' own tar (bsdtar): forward-slash entry names, and the launcher unpacks with the same tool
$zipName = "TriRod-$version-Windows-x64.zip"
$zip = Join-Path $outVersion $zipName
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
$entries = Get-ChildItem -LiteralPath $stage | ForEach-Object { $_.Name }
& $tar -a -c -f $zip -C $stage @entries
if ($LASTEXITCODE -ne 0) { throw "tar could not create $zip (exit $LASTEXITCODE)." }
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$size = (Get-Item -LiteralPath $zip).Length

$manifest = [ordered]@{
    format = 'trirod-release'
    schema = 1
    version = $version
    channel = 'windows-x64'
    repository = $UpdateRepo
    min_launcher = $MinLauncher
    min_updater = $MinUpdater
    required = [bool]$Required
    created = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    build = [ordered]@{ gamelib_id = $tools.gamelib_id; gamebuild_id = $tools.gamebuild_id; protocol = $tools.protocol }
    assets = @([ordered]@{ name = $zipName; kind = 'tools'; size = $size; sha256 = $hash })
}
$json = $manifest | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText((Join-Path $outVersion 'trirod-release.json'), $json + "`n", (New-Object Text.UTF8Encoding($false)))
Remove-Item -LiteralPath $stage -Recurse -Force

Write-Host "Release files for $version in $outVersion :"
Write-Host "  $zipName ($size bytes, SHA-256 $hash)"
Write-Host "  trirod-release.json"
Write-Host "Nothing was uploaded. Publish both files together in one GitHub Release of $UpdateRepo (tag $version),"
Write-Host "then check the update path as docs/RELEASE-WORKFLOW.md describes."
