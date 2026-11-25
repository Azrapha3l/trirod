[CmdletBinding()]
param(
    # A staged development package (setup/build.ps1)
    [string]$PackageDir = 'build\setup-dev',
    # Scratch folder for the test installations (inside build\, which git ignores); emptied first
    [string]$WorkDir = 'build\selfupdate-test'
)

# End-to-end test of the launcher's self-update (runtime/launcher/launcher_win.cpp, "self-update"; docs/RELEASE-WORKFLOW.md)
# with real processes and no network: an "old" installation copied from the package, and a local release
# (--update-from) whose launcher is built as the next patch version. Each scenario starts the old launcher with
# --update and checks what is running and installed afterwards. No game files are involved: the data folder holds
# only marker files standing in for settings, saves and caches, and must be byte-identical afterwards.
# The launcher windows appear on the desktop while it runs (about 3 minutes).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pkg = (Resolve-Path -LiteralPath (Join-Path $root $PackageDir)).Path
$work = Join-Path $root $WorkDir
$failures = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $script:failures++ }
}
function Launchers([string]$dir) {
    @(Get-CimInstance Win32_Process -Filter "Name='Trirod-Launcher.exe'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase) })
}
function Stop-Launchers([string]$dir) {
    foreach ($p in Launchers $dir) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
    for ($i = 0; $i -lt 40 -and @(Launchers $dir).Count; $i++) { Start-Sleep -Milliseconds 250 }
}
function Wait-Until([scriptblock]$cond, [int]$seconds) {
    $t = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $t) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 500 }
    return [bool](& $cond)
}
function Hash([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
function Tree-Hash([string]$dir) {  # (logs\ excluded: the launcher writes its build log there)
    (Get-ChildItem -LiteralPath $dir -Recurse -File | Where-Object { $_.FullName.Substring($dir.Length) -notmatch '^\\logs\\' } |
        Sort-Object FullName | ForEach-Object { $_.FullName.Substring($dir.Length) + '=' + (Hash $_.FullName) }) -join ';'
}
function Tools-Version([string]$install) { (Get-Content -LiteralPath (Join-Path $install 'tools\trirod-tools.json') -Raw | ConvertFrom-Json).version }

$tools = Get-Content -LiteralPath (Join-Path $pkg 'tools\trirod-tools.json') -Raw | ConvertFrom-Json
$oldVersion = $tools.version
if ($oldVersion -notmatch '^v(\d+)\.(\d+)\.(\d+)$') { throw "the package version $oldVersion is not v<x.y.z>" }
$newVersion = "v$($Matches[1]).$($Matches[2]).$([int]$Matches[3] + 1)"
Write-Host "old $oldVersion -> new $newVersion"

Stop-Launchers $work
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null

# The new launcher: the same source, built as the next version (its own build folder)
$newBuild = 'build/selfupdate-newlauncher'
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"
Push-Location $root
try {
    & make "B=$newBuild" "$newBuild/Trirod-Launcher.exe" "TRIROD_VERSION=$newVersion" 'UPDATE_REPO=' 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "building the $newVersion launcher failed" }
} finally { Pop-Location }
$newLauncher = Join-Path $root "$newBuild\Trirod-Launcher.exe"

# An installation like a release unpacks to (the allowlist of setup/make_release.ps1), at a given version
function New-Package([string]$dest, [string]$version, [string]$launcher) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    foreach ($n in @('trbuild.exe', 'Trirod-ShaderPrep.exe', 'trirod.ico', 'icon.png', 'tools')) {
        Copy-Item -LiteralPath (Join-Path $pkg $n) -Destination $dest -Recurse
    }
    Copy-Item -LiteralPath $launcher -Destination (Join-Path $dest 'Trirod-Launcher.exe')
    Get-ChildItem -LiteralPath $pkg -Filter '*.dll' -File | Where-Object { $_.Name -notin @('Trirod.dll', 'eowgame.dll') } |
        Copy-Item -Destination $dest
    $tj = Get-Content -LiteralPath (Join-Path $dest 'tools\trirod-tools.json') -Raw | ConvertFrom-Json
    $tj.version = $version
    $tj | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dest 'tools\trirod-tools.json') -Encoding utf8
}
# A local release: the zip and its manifest, as setup/make_release.ps1 writes them
function New-Release([string]$dir, [string]$stage, [string]$version, [string]$sha = '') {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $zip = Join-Path $dir "Trirod-Tools-$version.zip"
    $entries = Get-ChildItem -LiteralPath $stage | ForEach-Object { $_.Name }
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -a -c -f $zip -C $stage @entries
    if ($LASTEXITCODE -ne 0) { throw "tar failed for $zip" }
    if (-not $sha) { $sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() }
    $m = [ordered]@{ format = 'trirod-release'; schema = 1; version = $version; channel = 'windows-x64'; repository = 'local/test'
                     min_launcher = $version; min_updater = ''; required = $false
                     build = [ordered]@{ gamelib_id = $tools.gamelib_id; gamebuild_id = $tools.gamebuild_id; protocol = $tools.protocol }
                     assets = @([ordered]@{ name = (Split-Path $zip -Leaf); kind = 'tools'; size = (Get-Item $zip).Length; sha256 = $sha }) }
    [IO.File]::WriteAllText((Join-Path $dir 'trirod-release.json'), ($m | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
}
# The player's data folder: markers for settings, saves and caches (never touched by an update)
function New-Data([string]$dir) {
    foreach ($d in 'save\save_0', 'cache', 'extracted') { New-Item -ItemType Directory -Force -Path (Join-Path $dir $d) | Out-Null }
    Set-Content -LiteralPath (Join-Path $dir 'trirod-launcher.ini') -Value "# test`nsource=C:\nowhere" -Encoding ascii
    Set-Content -LiteralPath (Join-Path $dir 'eowrecomp.ini') -Value 'volume = 1.00' -Encoding ascii
    Set-Content -LiteralPath (Join-Path $dir 'save\save_0\save.meta') -Value 'save marker' -Encoding ascii
    Set-Content -LiteralPath (Join-Path $dir 'cache\shaderprep.json') -Value '{}' -Encoding ascii
}
function Start-Old([string]$install, [string]$data, [string]$release) {
    Start-Process -FilePath (Join-Path $install 'Trirod-Launcher.exe') -ArgumentList @('--dev', '--data', "`"$data`"", '--update-from', "`"$release`"", '--update') -PassThru
}

$oldLauncher = Join-Path $pkg 'Trirod-Launcher.exe'
$newStage = Join-Path $work 'new-package'
New-Package $newStage $newVersion $newLauncher

# ---------------------------------------------------------------- 1. the update: close, install, restart
Write-Host "`n1. update $oldVersion -> $newVersion (min_launcher $newVersion): the old launcher closes, the new one takes over"
$i1 = Join-Path $work 'install1'; $d1 = Join-Path $work 'data1'; $r1 = Join-Path $work 'release1'
New-Package $i1 $oldVersion $oldLauncher; New-Data $d1; New-Release $r1 $newStage $newVersion
$dataBefore = Tree-Hash $d1
$old = Start-Old $i1 $d1 $r1
$done = Wait-Until { (Tools-Version $i1) -eq $newVersion -and @(Launchers $i1 | Where-Object { $_.CommandLine -match '--updated' }).Count -eq 1 } 120
Check $done "tools $newVersion installed and a restarted launcher is running"
Start-Sleep 3
$running = @(Launchers $i1)
Check ($running.Count -eq 1) "exactly one launcher runs for the installation ($($running.Count))"
Check (-not (Get-Process -Id $old.Id -ErrorAction SilentlyContinue)) "the old launcher process ($($old.Id)) has closed"
Check ((Hash (Join-Path $i1 'Trirod-Launcher.exe')) -eq (Hash $newLauncher)) "the installed Trirod-Launcher.exe is the new one"
$cmd = if ($running.Count) { $running[0].CommandLine } else { '' }
Check ($cmd -match [regex]::Escape($d1) -and $cmd -match '--dev' -and $cmd -match "--updated $newVersion") "restarted with the same --data and --dev, plus --updated ($cmd)"
Check ($running.Count -and $running[0].ExecutablePath -ieq (Join-Path $i1 'Trirod-Launcher.exe')) "it runs from the installation, not the stage"
Check ((Hash (Join-Path $i1 "updates\$newVersion\previous\Trirod-Launcher.exe")) -eq (Hash $oldLauncher)) "the previous launcher is kept in updates\$newVersion\previous"
Check ((Tree-Hash $d1) -eq $dataBefore) "the data folder (settings, saves, caches) is unchanged"
$log = Get-Content -LiteralPath (Join-Path $i1 "updates\$newVersion\apply.log") -Raw -ErrorAction SilentlyContinue
Check ($log -match 'update complete') "apply.log records the completed update"
Stop-Launchers $i1

# ---------------------------------------------------------------- 2. no update loop after the restart
Write-Host "`n2. starting the updated launcher again with the same release: nothing to install"
$again = Start-Old $i1 $d1 $r1
Start-Sleep 12
Check (@(Launchers $i1).Count -eq 1 -and (Get-Process -Id $again.Id -ErrorAction SilentlyContinue)) "the launcher stays open and alone"
Check ((Hash (Join-Path $i1 'Trirod-Launcher.exe')) -eq (Hash $newLauncher)) "nothing was reinstalled"
Stop-Launchers $i1

# ---------------------------------------------------------------- 3. a package that fails verification
Write-Host "`n3. wrong SHA-256 in the manifest: refused, the old installation untouched and still running"
$i3 = Join-Path $work 'install3'; $d3 = Join-Path $work 'data3'; $r3 = Join-Path $work 'release3'
New-Package $i3 $oldVersion $oldLauncher; New-Data $d3; New-Release $r3 $newStage $newVersion ('0' * 64)
$before = Tree-Hash $i3
$p = Start-Old $i3 $d3 $r3
$log3 = Join-Path $d3 'logs\last-build.log'
Check (Wait-Until { (Test-Path $log3) -and ((Get-Content $log3 -Raw) -match 'failed verification') } 60) "the update failed verification"
Check ([bool](Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) "the old launcher is still open"
Check ((Tree-Hash $i3 | ForEach-Object { $_ -replace ';\\updates\\[^;]*', '' }) -eq ($before -replace ';\\updates\\[^;]*', '')) "the installation is unchanged"
Stop-Launchers $i3

# ---------------------------------------------------------------- 4. the new launcher can't start: refused before the hand-off
Write-Host "`n4. the package's launcher can't start (broken SDL3.dll): refused before anything is replaced"
$i4 = Join-Path $work 'install4'; $d4 = Join-Path $work 'data4'; $r4 = Join-Path $work 'release4'; $s4 = Join-Path $work 'broken-package'
New-Package $s4 $newVersion $newLauncher
[IO.File]::WriteAllBytes((Join-Path $s4 'SDL3.dll'), [byte[]](1..64))
New-Package $i4 $oldVersion $oldLauncher; New-Data $d4; New-Release $r4 $s4 $newVersion
$before = Hash (Join-Path $i4 'Trirod-Launcher.exe')
$p = Start-Old $i4 $d4 $r4
$log4 = Join-Path $d4 'logs\last-build.log'
Check (Wait-Until { (Test-Path $log4) -and ((Get-Content $log4 -Raw) -match "launcher does not start") } 90) "the staged launcher check failed"
Check ([bool](Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) "the old launcher is still open"
Check ((Hash (Join-Path $i4 'Trirod-Launcher.exe')) -eq $before -and (Tools-Version $i4) -eq $oldVersion) "the installation is unchanged"
Stop-Launchers $i4

# ---------------------------------------------------------------- 5. replacement fails half-way: rollback and restart
Write-Host "`n5. an installed file is in use during the swap: rolled back, the previous launcher restarts with the error"
$i5 = Join-Path $work 'install5'; $d5 = Join-Path $work 'data5'; $r5 = Join-Path $work 'release5'
New-Package $i5 $oldVersion $oldLauncher; New-Data $d5; New-Release $r5 $newStage $newVersion
$before = Tree-Hash $i5
# (open without delete sharing, as a running tool or an antivirus scan would: it can't be moved aside)
$lock = [IO.File]::Open((Join-Path $i5 'trbuild.exe'), 'Open', 'Read', 'Read')
try {
    $old = Start-Old $i5 $d5 $r5
    $back = Wait-Until { @(Launchers $i5 | Where-Object { $_.CommandLine -match '--update-error' }).Count -eq 1 } 120
} finally { $lock.Close() }
Check $back "the previous launcher was restarted with --update-error"
Start-Sleep 2
Check (@(Launchers $i5).Count -eq 1) "exactly one launcher runs"
Check ((Hash (Join-Path $i5 'Trirod-Launcher.exe')) -eq (Hash $oldLauncher) -and (Tools-Version $i5) -eq $oldVersion) "the previous installation is back in place"
Check (((Tree-Hash $i5) -replace ';\\updates\\[^;]*', '') -eq ($before -replace ';\\updates\\[^;]*', '')) "every installed file is as before"
$log5 = Get-Content -LiteralPath (Join-Path $i5 "updates\$newVersion\apply.log") -Raw -ErrorAction SilentlyContinue
Check ($log5 -match 'restoring the previous installation' -and $log5 -match 'previous launcher is running again') "apply.log records the rollback"
Stop-Launchers $i5

# ---------------------------------------------------------------- 6. a second launcher for the same folders
Write-Host "`n6. a second launcher for the same installation and data folder: refused"
$first = Start-Process -FilePath (Join-Path $i1 'Trirod-Launcher.exe') -ArgumentList @('--dev', '--data', "`"$d1`"") -PassThru
Start-Sleep 4
$second = Start-Process -FilePath (Join-Path $i1 'Trirod-Launcher.exe') -ArgumentList @('--dev', '--data', "`"$d1`"") -PassThru
Start-Sleep 4
$second.Refresh()
Check ($second.HasExited -or $second.MainWindowTitle -eq 'Trirod Launcher') "the second one only shows 'already running' ($($second.MainWindowTitle))"
$first.Refresh()
Check ($first.MainWindowTitle -eq 'TriRod') "the first one keeps its window"
Stop-Launchers $i1

if ($failures) { Write-Host "`n$failures check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`ntest_self_update: all checks passed"
