[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    # A local development installation is also a data folder: the player data at its top level (game files, the
    # built game, saves, caches, logs) is skipped instead of refused. Release archives never pass this.
    [switch]$AllowUserData
)

# Refuses a package that holds the game's files or anything made from them (CLAUDE.md, docs/RELEASE-WORKFLOW.md):
# extracted ExeFS/RomFS, packages, keys, shader archives or an uncurated shader catalog, the player's built game code.
$ErrorActionPreference = 'Stop'
$pkg = (Resolve-Path -LiteralPath $PackageDir).Path
$userTop = @('extracted', 'cache', 'save', 'saves', 'logs', 'updates', 'fonts', 'rando', 'Trirod.exe', 'Trirod.exe.id',
             'trirod-install.json', 'trirod-launcher.ini', 'eowrecomp.ini', 'eowrecomp.log')
$badDirs = @('extracted', 'exefs', 'romfs', 'xci_secure', 'nso_raw', '.gamebuild')
$badNames = @('prod.keys', 'title.keys', 'catalog.bin', 'romfs.bin', 'romfs.src', 'main.npdm', 'Trirod.dll',
              'Trirod.dll.id', 'eowgame.dll', 'eowgame.dll.id', 'Trirod.exe', 'Trirod.exe.id', 'vk_pipelines.bin',
              'recipes.bin')
$badExt = @('.xci', '.nsp', '.nca', '.nso', '.nsz', '.xcz', '.bfsha', '.bfttf', '.bntx', '.bfres', '.keys', '.spv')
$warmDirs = @('tools\shader_catalog', 'tools\shader_catalog\warm', 'tools\shader_catalog\warm\code')
$warmRecipes = 'tools\shader_catalog\warm\recipes.bin'
$warmReadme = 'tools\shader_catalog\warm\README.md'

$problems = New-Object System.Collections.Generic.List[string]
Get-ChildItem -LiteralPath $pkg -Force | ForEach-Object {
    if ($AllowUserData -and ($userTop -contains $_.Name)) { return }
    $items = @($_)
    if ($_.PSIsContainer) { $items += Get-ChildItem -LiteralPath $_.FullName -Recurse -Force }
    foreach ($item in $items) {
        $rel = $item.FullName.Substring($pkg.Length).TrimStart('\')
        if ($item.PSIsContainer) {
            if (($badDirs -contains $item.Name) -or
                ($rel.StartsWith('tools\shader_catalog', [StringComparison]::OrdinalIgnoreCase) -and $warmDirs -notcontains $rel)) {
                $problems.Add("folder $rel")
            }
        } else {
            $portableCode = $rel -match '^tools\\shader_catalog\\warm\\code\\[0-9a-f]{16}-[0-9a-f]+\.bin$'
            $portableRecipe = $rel -ieq $warmRecipes
            $portableReadme = $rel -ieq $warmReadme
            $insideWarm = $rel.StartsWith('tools\shader_catalog\warm\', [StringComparison]::OrdinalIgnoreCase)
            if (($badNames -contains $item.Name -and -not $portableRecipe) -or
                ($badExt -contains $item.Extension.ToLowerInvariant()) -or
                ($insideWarm -and -not ($portableRecipe -or $portableCode -or $portableReadme))) {
                $problems.Add("file $rel")
            }
        }
    }
}
if ($problems.Count) {
    $list = ($problems | Select-Object -First 20) -join "`n  "
    throw "The package at $pkg contains game data or files made from it; refusing it:`n  $list"
}
Write-Host "Package check passed: no game files in $pkg"
