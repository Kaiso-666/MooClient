# Builds the Release exe and wraps it in the Inno Setup installer.
# Output: dist\MooClientSetup-<version>.exe
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$isccCandidates = @(
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe"
)
$iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    throw "ISCC.exe not found. Install Inno Setup 6: https://jrsoftware.org/isdl.php"
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Config Release
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
}

$exe = Join-Path $root "build\bin\MooClient.exe"
if (-not (Test-Path $exe)) { throw "MooClient.exe is missing; build first." }

$version = (Get-Item $exe).VersionInfo.ProductVersion
if (-not $version) { throw "MooClient.exe has no version resource." }

& $iscc (Join-Path $root "installer\moonsetup.iss")
if ($LASTEXITCODE -ne 0) { throw "Installer build failed with exit code $LASTEXITCODE" }

$setup = Join-Path $root "dist\MooClientSetup-$version.exe"
if (-not (Test-Path $setup)) { throw "Expected installer was not produced: $setup" }

Write-Host ""
Write-Host ("Installer: {0} ({1:N2} MB)" -f $setup, ((Get-Item $setup).Length / 1MB))
