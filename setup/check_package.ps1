[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    # A local development installation is also a data folder: the player data at its top level (game files, the
    # built game, saves, caches, logs) is skipped instead of refused. Release archives never pass this.
    [switch]$AllowUserData
)

# Refuses a package that holds the game's files or anything made from them (CLAUDE.md, docs/RELEASE-WORKFLOW.md):
# extracted ExeFS/RomFS, packages, keys, shader archives or the shader catalog, the player's built game code.
$ErrorActionPreference = 'Stop'
$pkg = (Resolve-Path -LiteralPath $PackageDir).Path
$userTop = @('extracted', 'cache', 'save', 'saves', 'logs', 'updates', 'fonts', 'rando', 'Trirod.exe', 'Trirod.exe.id',
             'trirod-install.json', 'trirod-launcher.ini', 'eowrecomp.ini', 'eowrecomp.log')
$badDirs = @('extracted', 'exefs', 'romfs', 'shader_catalog', 'xci_secure', 'nso_raw', '.gamebuild')
$badNames = @('prod.keys', 'title.keys', 'catalog.bin', 'romfs.bin', 'romfs.src', 'main.npdm', 'Trirod.dll',
              'Trirod.dll.id', 'eowgame.dll', 'eowgame.dll.id', 'Trirod.exe', 'Trirod.exe.id', 'vk_pipelines.bin',
              'recipes.bin')
$badExt = @('.xci', '.nsp', '.nca', '.nso', '.nsz', '.xcz', '.bfsha', '.bfttf', '.bntx', '.bfres', '.keys', '.spv')

$problems = New-Object System.Collections.Generic.List[string]
Get-ChildItem -LiteralPath $pkg -Force | ForEach-Object {
    if ($AllowUserData -and ($userTop -contains $_.Name)) { return }
    $items = @($_)
    if ($_.PSIsContainer) { $items += Get-ChildItem -LiteralPath $_.FullName -Recurse -Force }
    foreach ($item in $items) {
        $rel = $item.FullName.Substring($pkg.Length).TrimStart('\')
        if ($item.PSIsContainer) {
            if ($badDirs -contains $item.Name) { $problems.Add("folder $rel") }
        } elseif (($badNames -contains $item.Name) -or ($badExt -contains $item.Extension.ToLowerInvariant())) {
            $problems.Add("file $rel")
        }
    }
}
if ($problems.Count) {
    $list = ($problems | Select-Object -First 20) -join "`n  "
    throw "The package at $pkg contains game data or files made from it; refusing it:`n  $list"
}
Write-Host "Package check passed: no game files in $pkg"
