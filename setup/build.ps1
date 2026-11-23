[CmdletBinding()]
param(
    [string]$BuildDir = 'build\win-native',
    [string]$OutputDir = 'build\setup-dev',
    [int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 1),
    # owner/name of the GitHub repository whose Releases the launcher checks (release builds only; empty: the
    # launcher never goes online). docs/RELEASE-WORKFLOW.md
    [string]$UpdateRepo = '',
    # GUI-subsystem executables (no console windows), as published releases are built
    [switch]$Release,
    [switch]$SkipCompile
)

# Builds the Windows launcher, the headless tools (trbuild.exe, Trirod-ShaderPrep.exe), the runtime link kit and the
# trgen generator from this checkout, then stages a runnable installation (setup/pack_tools.ps1). Local only: it
# downloads nothing and uploads nothing. Run the result with: build\setup-dev\Trirod-Launcher.exe --dev
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $root
try {
    $make = '[local path omitted]
    $msysBin = '[local path omitted]
    $ucrtBin = '[local path omitted]
    $python = Join-Path $root 'tools\venv\Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $python -PathType Leaf)) {
        $python = Join-Path $root 'build\gen-venv\Scripts\python.exe'
    }

    foreach ($required in @(
        $make,
        (Join-Path $ucrtBin 'clang.exe'),
        (Join-Path $ucrtBin 'windres.exe'),
        (Join-Path $ucrtBin 'dlltool.exe'),
        (Join-Path $ucrtBin 'objdump.exe'),
        (Join-Path $root 'build\win\deps\ucrt64\bin\SDL3.dll'),
        (Join-Path $root 'build\win\deps\ffmpeg\bin\avcodec-62.dll'),
        (Join-Path $root 'build\win\diag-prefix\include\vma\vk_mem_alloc.h'),
        (Join-Path $root 'build\zig\zig.exe'),
        $python
    )) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Local build prerequisite is missing: $required. This script does not download dependencies."
        }
    }

    if (-not $SkipCompile) {
        # Refresh the game's local generator from the current lifter sources. PyInstaller must
        # already be installed in the local Python environment; no package manager is invoked.
        & $python -m PyInstaller --noconfirm --clean --log-level WARN `
            --distpath (Join-Path $root 'build\helper') `
            --workpath (Join-Path $root 'build\pyinstaller') `
            tools\trgen.spec
        if ($LASTEXITCODE -ne 0) { throw "Local trgen build failed with exit code $LASTEXITCODE." }
        & $python tools\pe_no_cfg.py build\helper\trgen\trgen.exe
        if ($LASTEXITCODE -ne 0) { throw "Could not prepare the local trgen executable (exit $LASTEXITCODE)." }

        $env:PATH = "$ucrtBin;$msysBin;$env:PATH"
        $makeBuildDir = $BuildDir.Replace('\', '/')
        $makeArgs = @("-j$Jobs", "B=$makeBuildDir", 'DEPS=build/win/diag-prefix', 'WIN_DEPS=build/win/deps',
                      'ZIG=build/zig/zig.exe', "UPDATE_REPO=$UpdateRepo")
        if ($Release) { $makeArgs += 'RELEASE=1' }
        & $make @makeArgs 'all'
        if ($LASTEXITCODE -ne 0) { throw "TriRod build failed with exit code $LASTEXITCODE." }
    }

    & (Join-Path $PSScriptRoot 'pack_tools.ps1') -BuildDir $BuildDir -OutputDir $OutputDir
    Write-Host "TriRod package ready at $(Join-Path $root $OutputDir)"
    Write-Host "Development run: $(Join-Path $root $OutputDir)\Trirod-Launcher.exe --dev"
}
finally {
    Pop-Location
}
