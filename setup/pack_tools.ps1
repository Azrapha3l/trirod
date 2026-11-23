[CmdletBinding()]
param(
    [string]$BuildDir = "build\win-native",
    [string]$OutputDir = "build\setup-dev"
)

# Stages a runnable Windows installation from a build (docs/RELEASE-WORKFLOW.md): the launcher, the headless tools it
# runs, the runtime DLLs and tools/ (trgen, zig, the link kit). It is the same layout a release archive unpacks to
# (setup/make_release.ps1 zips this folder). Game files and anything made from them are never part of it: the DLLs are
# an explicit list, and the staged folder is checked for game data before it is used.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root $BuildDir
$out = Join-Path $root $OutputDir
$stageRoot = Join-Path $root 'build\.trirod-tools-stage'
$tools = Join-Path $stageRoot 'tools'

function Require-Path([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing $Description at '$Path'. Build the Windows runtime/tools first (setup/build.ps1)."
    }
}

Require-Path (Join-Path $build 'Trirod-Launcher.exe') 'Trirod launcher executable'
Require-Path (Join-Path $build 'trbuild.exe') 'game build tool'
Require-Path (Join-Path $build 'Trirod-ShaderPrep.exe') 'shader preparation tool'
Require-Path (Join-Path $build 'trirod-build.json') 'build identifiers (make all writes them)'
Require-Path (Join-Path $build 'link-kit\eowruntime.a') 'runtime link kit'
Require-Path (Join-Path $build 'link-kit\lib\eowrecomp.lib') 'runtime import library'
Require-Path (Join-Path $build 'link-kit\lib\stdc++.lib') 'libstdc++ import library (make all copies it)'
Require-Path (Join-Path $root 'build\helper\trgen\trgen.exe') 'game-code generator'
Require-Path (Join-Path $root 'build\zig\zig.exe') 'Zig compiler'

# Keep package output inside build/ and refuse paths that could alias the source checkout.
$resolvedOut = [IO.Path]::GetFullPath($out)
$resolvedBuild = [IO.Path]::GetFullPath((Join-Path $root 'build'))
if (-not $resolvedOut.StartsWith($resolvedBuild + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDir must remain inside '$resolvedBuild'."
}

New-Item -ItemType Directory -Force -Path $out | Out-Null
if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $tools | Out-Null

# These are the files gamebuild.c resolves through rt_resource (runtime/paths.c: <exe dir>\tools\ first) from
# trbuild.exe, which sits at the package root beside the launcher.
Copy-Item -LiteralPath (Join-Path $root 'build\helper\trgen') -Destination $tools -Recurse
Copy-Item -LiteralPath (Join-Path $root 'build\zig') -Destination (Join-Path $tools 'zig') -Recurse
Copy-Item -LiteralPath (Join-Path $build 'link-kit') -Destination $tools -Recurse

# Zig's Windows linker consumes import libraries from link-kit/lib. Keep the original MSYS
# archives and add canonical .lib aliases for names used by link.args.
$linkLibs = Join-Path $tools 'link-kit\lib'
foreach ($libRoot in @((Join-Path $root 'build\win\deps\ucrt64\lib'),
                       (Join-Path $root 'build\win\deps\ffmpeg\lib'))) {
    if (-not (Test-Path -LiteralPath $libRoot -PathType Container)) { continue }
    Get-ChildItem -LiteralPath $libRoot -File -Recurse | Where-Object {
        $_.Extension -in @('.lib', '.a') -or $_.Name.EndsWith('.dll.a', [StringComparison]::OrdinalIgnoreCase)
    } | ForEach-Object {
        if ($_.Name.EndsWith('.dll.a', [StringComparison]::OrdinalIgnoreCase)) {
            $alias = $_.Name -replace '^lib', '' -replace '\.dll\.a$', '.lib'
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $linkLibs $alias) -Force
        } else {
            Copy-Item -LiteralPath $_.FullName -Destination $linkLibs -Force
        }
    }
}

$gameLibSource = Join-Path $tools 'gamelib-src'
New-Item -ItemType Directory -Force -Path $gameLibSource | Out-Null
Copy-Item -LiteralPath @(
    (Join-Path $root 'runtime\cpu.h'),
    (Join-Path $root 'runtime\simd.h'),
    (Join-Path $root 'runtime\simd_fp.h'),
    (Join-Path $root 'runtime\gamelib.h'),
    (Join-Path $root 'runtime\modules.h'),
    (Join-Path $root 'tools\gamelib_entry.c'),
    (Join-Path $root 'runtime\hle\hle_symbols.txt')
) -Destination $gameLibSource

# Retain readable local generator/lifter sources for diagnosis and development iteration.
$source = Join-Path $tools 'source'
New-Item -ItemType Directory -Force -Path $source | Out-Null
foreach ($relative in @('runtime', 'tools', 'setup', 'Makefile', 'docs\ARCHITECTURE.md')) {
    $from = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $from)) { continue }
    $to = Join-Path $source $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $to -Parent) | Out-Null
    if (Test-Path -LiteralPath $from -PathType Container) {
        New-Item -ItemType Directory -Force -Path $to | Out-Null
        Get-ChildItem -LiteralPath $from -Force -File -Recurse |
            Where-Object { $_.FullName -notmatch '[\\/](venv|__pycache__|\.pytest_cache|\.mypy_cache)[\\/]' } |
            ForEach-Object {
                $relativeFile = $_.FullName.Substring($from.Length).TrimStart([char[]]@('\', '/'))
                $dest = Join-Path $to $relativeFile
                New-Item -ItemType Directory -Force -Path (Split-Path $dest -Parent) | Out-Null
                Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
            }
    } else {
        Copy-Item -LiteralPath $from -Destination $to -Force
    }
}

# tools/trirod-tools.json: the launcher reads the version and the identifiers these tools build (Makefile
# trirod-build.json) and compares them with the installed Trirod.dll.id / Trirod.exe.id.
$ids = Get-Content -LiteralPath (Join-Path $build 'trirod-build.json') -Raw | ConvertFrom-Json
[ordered]@{
    product = 'TriRod'
    format = 2
    version = $ids.version
    gamelib_id = $ids.gamelib_id
    gamebuild_id = $ids.gamebuild_id
    protocol = $ids.protocol
    builtFrom = (Get-Date).ToUniversalTime().ToString('o')
    sourceRoot = 'source/'
    runtimeCompiler = 'zig/zig.exe'
    gameCodeGenerator = 'trgen/trgen.exe'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $tools 'trirod-tools.json') -Encoding utf8

# The package root: launcher, tools, the DLLs Trirod.exe / Trirod-ShaderPrep.exe / the launcher load. An explicit
# list: the build folder can hold game code built from a player's files (eowgame.dll, Trirod.dll), never shipped.
$runtimeDlls = @('SDL3', 'libshaderc_shared', 'libSPIRV-Tools-opt', 'libSPIRV-Tools', 'glslang', 'SPIRV',
                 'libglslang-default-resource-limits', 'libstdc++-6', 'libgcc_s_seh-1', 'libwinpthread-1',
                 'libiconv-2', 'libcharset-1')
$outTools = Join-Path $out 'tools'
if (Test-Path -LiteralPath $outTools) { Remove-Item -LiteralPath $outTools -Recurse -Force }
Copy-Item -LiteralPath $tools -Destination $out -Recurse
foreach ($exe in @('Trirod-Launcher.exe', 'trbuild.exe', 'Trirod-ShaderPrep.exe')) {
    Copy-Item -LiteralPath (Join-Path $build $exe) -Destination (Join-Path $out $exe) -Force
}
foreach ($name in $runtimeDlls) {
    $dll = Join-Path $build "$name.dll"
    Require-Path $dll "runtime library $name.dll (make windows-runtime-deps copies it)"
    Copy-Item -LiteralPath $dll -Destination $out -Force
}
Get-ChildItem -LiteralPath (Join-Path $root 'build\win\deps\ffmpeg\bin') -Filter '*.dll' -File |
    Copy-Item -Destination $out -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets\trirod.ico') -Destination (Join-Path $out 'trirod.ico') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets\trirod-staff.png') -Destination (Join-Path $out 'icon.png') -Force
# Files of the old Setup that the new launcher no longer uses
foreach ($old in @('Tools.zip', 'eowsetup.exe', 'eowgame.dll', 'eowgame.dll.id')) {
    Remove-Item -LiteralPath (Join-Path $out $old) -Force -ErrorAction SilentlyContinue
}

& (Join-Path $PSScriptRoot 'check_package.ps1') -PackageDir $out -AllowUserData
Write-Host "Local TriRod installation staged: $out"
