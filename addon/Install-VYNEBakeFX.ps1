# VYNE Native BakeFX installer. Run from the extracted ZIP while FiveM is closed.
# Existing files are backed up before changing anything.
param(
    [string]$AddonDirectory = "",
    [string]$ShaderDirectory = ""
)
$ErrorActionPreference = "Stop"
$source = Split-Path -Parent $MyInvocation.MyCommand.Path
$plugins = Join-Path $env:LOCALAPPDATA "FiveM\FiveM.app\plugins"
$ini = Join-Path $plugins "BetterVideoExport.ini"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"

if (!(Test-Path $plugins -PathType Container) -or !(Test-Path $ini)) {
    throw "Existing FiveM.app/plugins and BetterVideoExport.ini are required."
}
if (Get-Process -Name FiveM*,GTA5* -ErrorAction SilentlyContinue) {
    throw "Close FiveM and GTA V before installation."
}
function Copy-WithBackup([string]$src, [string]$target) {
    if (!(Test-Path -LiteralPath $src -PathType Leaf)) { throw "Missing $src" }
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        Copy-Item -LiteralPath $target -Destination ($target + ".backup-" + $stamp) -Force
    }
    Copy-Item -LiteralPath $src -Destination $target -Force
    Write-Host "Installed $target"
}

# Recover the add-on directory from the known-working previous add-on install.
if (!$AddonDirectory) {
    $roots = @(
      $plugins,
      (Join-Path $env:LOCALAPPDATA "FiveM\FiveM.app"),
      "E:\SteamLibrary\steamapps\common\Grand Theft Auto V"
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Container } | Select-Object -Unique
    $matches = @()
    foreach ($root in $roots) {
        $matches += @(Get-ChildItem -LiteralPath $root -Filter VYNE_BakeFX.addon64 -Recurse -File -ErrorAction SilentlyContinue)
    }
    $matches = @($matches | Sort-Object FullName -Unique)
    if ($matches.Count -ne 1) {
        throw "Could not unambiguously locate the existing ReShade add-on. Run again with -AddonDirectory 'path to your active ReShade add-on folder'."
    }
    $AddonDirectory = $matches[0].DirectoryName
}
if (!(Test-Path -LiteralPath $AddonDirectory -PathType Container)) {
    throw "Invalid ReShade add-on directory: $AddonDirectory"
}
# Find an existing ReShade shader directory. Custom search paths can be supplied.
if (!$ShaderDirectory) {
    $possibilities = @(
        (Join-Path $AddonDirectory "reshade-shaders\Shaders"),
        (Join-Path (Split-Path $AddonDirectory) "reshade-shaders\Shaders"),
        (Join-Path $plugins "reshade-shaders\Shaders")
    )
    $ShaderDirectory = @($possibilities | Where-Object {
        Test-Path -LiteralPath $_ -PathType Container
    } | Select-Object -First 1)
}
if (!$ShaderDirectory) {
    throw "ReShade shader search directory not found. Run with -ShaderDirectory 'your ReShade shader folder' (as configured in ReShade.ini)."
}
Copy-WithBackup (Join-Path $source "BetterVideoExport.asi") (Join-Path $plugins "BetterVideoExport.asi")
Copy-WithBackup (Join-Path $source "VYNE_BakeFX.addon64") (Join-Path $AddonDirectory "VYNE_BakeFX.addon64")
Copy-WithBackup (Join-Path $source "VYNE_ExportMotion.fx") (Join-Path $ShaderDirectory "VYNE_ExportMotion.fx")

# Merge settings into the existing INI without losing NVENC / 4K60 options.
Copy-Item -LiteralPath $ini -Destination ($ini + ".backup-" + $stamp) -Force
$lines = [System.Collections.Generic.List[string]]::new()
foreach ($line in [System.IO.File]::ReadAllLines($ini)) { $lines.Add($line) }
function Set-INI([string]$section, [string]$key, [string]$value) {
    $begin = -1
    for ($i=0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -ieq "[$section]") { $begin=$i; break }
    }
    if ($begin -lt 0) {
        $lines.Add("")
        $lines.Add("[$section]")
        $lines.Add("$key=$value")
        return
    }
    $end = $lines.Count
    for ($i=$begin+1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[.+\]') { $end=$i; break }
    }
    for ($i=$begin+1; $i -lt $end; $i++) {
        if ($lines[$i] -match ("^\s*" + [regex]::Escape($key) + "\s*=")) {
            $lines[$i] = "$key=$value"
            return
        }
    }
    $lines.Insert($end, "$key=$value")
}
Set-INI "ReShadeBake" "Enable" "1"
Set-INI "ReShadeBake" "ApplyEffects" "1"
Set-INI "ReShadeBake" "OpticalBlur" "1"
Set-INI "ReShadeBake" "CaptureSnapshots" "0"
Set-INI "ReShadeBake" "TargetCandidate" "0"
Set-INI "Capture" "EveDx11" "0"
Set-INI "MotionBlur" "EditorTimecycleBlur" "0"
Set-INI "MotionBlur" "EditorExtraTimecycle" "0"
[System.IO.File]::WriteAllLines($ini, $lines, [System.Text.UTF8Encoding]::new($false))
Write-Host "Export FX and optical blur enabled. Previous experiments disabled."
Write-Host "Restart FiveM. Use a short clip for the first export."
Write-Host "If needed, restore the .backup-$stamp files or set [ReShadeBake] ApplyEffects=0."
