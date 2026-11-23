param(
    [string]$RuntimeDir = "build\win-native",
    [string]$OutputDir = "build\win\dist\EoWRecompSetup"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $root
$runtime = Join-Path $root $RuntimeDir
$out = Join-Path $root $OutputDir
$deps = Join-Path $root "build\win\deps"
$payload = Join-Path $out "payload"
$zig = Join-Path $root "build\zig"
$generator = Join-Path $root "build\helper\trgen"

foreach ($required in @(
    (Join-Path $runtime "Trirod-Launcher.exe"),
    (Join-Path $runtime "link-kit\eowruntime.a"),
    (Join-Path $runtime "link-kit\lib\eowrecomp.lib"),
    (Join-Path $runtime "link-kit\link.args"),
    (Join-Path $zig "zig.exe"),
    (Join-Path $generator "trgen.exe"),
    (Join-Path $PSScriptRoot "setup.cpp")
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required build input is missing: $required"
    }
}

$clangxx = (Get-Command clang++.exe -ErrorAction Stop).Source
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Path $payload -Force | Out-Null

Copy-Item -LiteralPath (Join-Path $runtime "Trirod-Launcher.exe") -Destination $payload
Copy-Item -LiteralPath (Join-Path $runtime "link-kit") -Destination $payload -Recurse
$linkLibs = Join-Path $payload "link-kit\lib"
New-Item -ItemType Directory -Path $linkLibs -Force | Out-Null
foreach ($libDir in @((Join-Path $deps "ucrt64\lib"), (Join-Path $deps "ffmpeg\lib"))) {
    if (Test-Path -LiteralPath $libDir -PathType Container) {
        Get-ChildItem -LiteralPath $libDir -File -Recurse |
            Where-Object { $_.Name.EndsWith(".lib") -or $_.Name.EndsWith(".a") } |
            Copy-Item -Destination $linkLibs -Force
    }
}
# Zig's Windows driver prefers import-library names like vulkan-1.lib. MSYS2 calls these lib*.dll.a;
# ship both the original archive and the canonical .lib alias so -l resolves the DLL import library first.
$msysPrefix = Split-Path (Split-Path $clangxx -Parent) -Parent
$msysLibDir = Join-Path $msysPrefix "lib"
if (Test-Path -LiteralPath $msysLibDir -PathType Container) {
    Get-ChildItem -LiteralPath $msysLibDir -Filter "*.dll.a" -File | ForEach-Object {
        $name = $_.Name -replace '^lib', '' -replace '\.dll\.a$', '.lib'
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $linkLibs $name) -Force
    }
}
Get-ChildItem -LiteralPath $runtime -Filter "*.dll" -File |
    Where-Object { $_.Name -ne "Trirod.dll" } | Copy-Item -Destination $payload
Copy-Item -LiteralPath $generator -Destination (Join-Path $payload "trgen") -Recurse
Copy-Item -LiteralPath $zig -Destination (Join-Path $payload "zig") -Recurse

$headers = Join-Path $payload "gamelib-src"
New-Item -ItemType Directory -Path $headers -Force | Out-Null
Copy-Item runtime\cpu.h,runtime\simd.h,runtime\simd_fp.h,runtime\gamelib.h,runtime\modules.h,tools\gamelib_entry.c,runtime\hle\hle_symbols.txt -Destination $headers
Copy-Item third_party\fonts\Baloo2\Baloo2-SemiBold.ttf,README.md,LICENSE -Destination $payload
Copy-Item docs\icon\icon_1024.png -Destination (Join-Path $payload "icon.png")

$setupExe = Join-Path $out "EoWRecompSetup.exe"
& $clangxx -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 '-Wl,--subsystem,windows' `
    -o $setupExe (Join-Path $PSScriptRoot "setup.cpp") -lole32 -lshell32 -lcomdlg32 -luuid
if ($LASTEXITCODE -ne 0) { throw "Couldn't compile EoWRecompSetup.exe" }
Copy-Item -LiteralPath (Join-Path $runtime "libgcc_s_seh-1.dll"),(Join-Path $runtime "libstdc++-6.dll") -Destination $out

Write-Output "Created $out"
