# Packs the shipping build. The research tooling is compiled out here, not
# merely hidden: with the flag at 0 the menu rows and the search functions
# never reach the binary, so nothing about them survives in the file.
#
# The flag is flipped by rewriting version.h and restoring it afterwards.
# Passing a define through MSBuild is the tidier idea and does not work -
# DefineConstants is a C# property, and the C++ equivalent is per-project.
# Editing the header is blunt, but it is the thing the compiler actually reads.
param(
    [string]$Version = "0.17.0",
    # Where the zip and the loose .asi are written. Defaults to a sibling of the
    # repo so the release folder never lands inside it.
    [string]$ReleaseDir = (Join-Path (Split-Path $PSScriptRoot -Parent) "Trinity-release"),
    # Explicit cmake.exe. Only needed when the search below cannot find one.
    [string]$CMake = ""
)

$ErrorActionPreference = "Stop"

# cmake is often not on PATH on a Windows dev box - it arrives bundled inside
# Visual Studio or as a Python wheel. Look in the usual places rather than
# hardcoding one machine's layout.
function Resolve-CMake {
    if ($CMake) {
        if (-not (Test-Path $CMake)) { throw "-CMake `"$CMake`" does not exist" }
        return $CMake
    }
    if ($env:TRINITY_CMAKE -and (Test-Path $env:TRINITY_CMAKE)) { return $env:TRINITY_CMAKE }
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $globs = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:LOCALAPPDATA\Packages\*\LocalCache\Roaming\Python\Python*\site-packages\cmake\data\bin\cmake.exe",
        "$env:APPDATA\Python\Python*\site-packages\cmake\data\bin\cmake.exe"
    )
    foreach ($g in $globs) {
        $hit = Get-ChildItem $g -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw "cmake.exe not found. Put it on PATH, set TRINITY_CMAKE, or pass -CMake <path>."
}

$cmake   = Resolve-CMake
$root    = $PSScriptRoot
$relDir  = $ReleaseDir
$binOut  = "$root\build\Release\Trinity.asi"
$verH    = "$root\src\core\version.h"

if (-not (Test-Path $relDir)) { New-Item -ItemType Directory -Path $relDir | Out-Null }
Write-Host "cmake  : $cmake"
Write-Host "release: $relDir"

$backup = [IO.File]::ReadAllText($verH)
try {
    $off = $backup -replace "#define TRINITY_MARKER_RESEARCH [01]", "#define TRINITY_MARKER_RESEARCH 0"
    [IO.File]::WriteAllText($verH, $off, (New-Object Text.UTF8Encoding $false))

    Write-Host "== building (research tooling off) =="
    Push-Location "$root\build"
    $log = & $cmake --build . --config Release --clean-first 2>&1 | Out-String
    Pop-Location
    $errs = ($log -split "`n") | Where-Object { $_ -match ": error |: fatal " }
    if ($errs) { $errs | Select-Object -First 8 | ForEach-Object { Write-Host $_ }; throw "build failed" }
}
finally {
    [IO.File]::WriteAllText($verH, $backup, (New-Object Text.UTF8Encoding $false))
    Write-Host "version.h restored"
}

if (-not (Test-Path $binOut)) { throw "no binary produced" }
if (((Get-Date) - (Get-Item $binOut).LastWriteTime).TotalMinutes -gt 15) {
    throw "binary is stale - the build did not actually run"
}

Write-Host "== auditing the shipping binary =="
$bytes = [IO.File]::ReadAllBytes($binOut)
$ascii = [Text.Encoding]::ASCII.GetString($bytes)
$utf16 = [Text.Encoding]::Unicode.GetString($bytes)
$bad   = @()
foreach ($w in "VTweak","vtweak","OptiScaler","CrimsonRoute","Orcax",
                "money/probe","marker/search","Marker Search","3289") {
    $n = ([regex]::Matches($ascii,[regex]::Escape($w))).Count +
         ([regex]::Matches($utf16,[regex]::Escape($w))).Count
    Write-Host ("  {0,-16} {1}" -f $w, $n)
    if ($n -gt 0) { $bad += $w }
}
if ($bad.Count) { throw ("shipping binary still carries: " + ($bad -join ", ")) }

Write-Host "== packing =="
$stage = Join-Path $env:TEMP "trinity-pack"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item $binOut $stage
# Languages\ is NOT shipped. Every translation is embedded in the binary
# (i18n_embedded.h), and Discover() lets a file on disk WIN over the
# embedded table - so shipping our own INIs shadowed the built-in ones
# with older data. It also made one line untranslatable: the INI splits
# on the first "=", and the English key "World units. Tighter = fewer
# false positives..." contains one, so that row could never match.
# The loader stays, so a player's own Languages\*.ini still overrides us.

Get-ChildItem $relDir -Filter "Trinity-*.zip" | Remove-Item -Force
$zip = Join-Path $relDir "Trinity-$Version-CD2.00.00.zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
Copy-Item $binOut "$relDir\Trinity.asi" -Force
Remove-Item $stage -Recurse -Force

$h = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Host ""
Write-Host "zip    : $zip"
Write-Host "size   : $([math]::Round((Get-Item $zip).Length/1KB)) KB"
Write-Host "sha256 : $h"