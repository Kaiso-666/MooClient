# Downloads the build dependencies into third_party/.
# No admin rights, no package manager: .nupkg files are just zips.
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$thirdParty = Join-Path $root "third_party"

function Get-NuGetPackage {
    param(
        [string]$Id,
        [string]$Version,
        [string]$Destination
    )

    if (-not $Version) {
        $index = Invoke-RestMethod "https://api.nuget.org/v3-flatcontainer/$Id/index.json"
        $Version = ($index.versions | Where-Object { $_ -notmatch "-" } | Select-Object -Last 1)
    }
    if (-not $Version) { throw "Could not resolve a version for $Id." }

    $url = "https://api.nuget.org/v3-flatcontainer/$Id/$Version/$Id.$Version.nupkg"
    $zip = Join-Path $env:TEMP "$Id.$Version.zip"

    Write-Host "Downloading $Id $Version ..."
    curl.exe -L --retry 5 --retry-delay 2 -o $zip $url
    if ($LASTEXITCODE -ne 0) { throw "Download failed: $url" }

    if (Test-Path $Destination) { Remove-Item $Destination -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Expand-Archive -Path $zip -DestinationPath $Destination -Force
    Remove-Item $zip -Force
}

# --- WebView2 SDK -----------------------------------------------------------
$webview2 = Join-Path $thirdParty "webview2"
if ($Force -or -not (Test-Path (Join-Path $webview2 "build\native\include\WebView2.h"))) {
    Get-NuGetPackage -Id "microsoft.web.webview2" -Destination $webview2
}

# --- Windows SDK headers + x64 libs ----------------------------------------
# The VS "Windows 11 SDK" component is sometimes skipped even when selected.
# Microsoft publishes the same headers/libs as NuGet packages, which lets a
# standard user build without installing the SDK machine-wide.
$winsdk = Join-Path $thirdParty "winsdk"
$headers = Join-Path $winsdk "base\c\Include"
$libs = Join-Path $winsdk "x64\c\um\x64"

if ($Force -or -not (Test-Path $headers) -or -not (Test-Path $libs)) {
    $index = Invoke-RestMethod "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/index.json"
    $version = ($index.versions | Where-Object { $_ -notmatch "preview" } | Select-Object -Last 1)

    Get-NuGetPackage -Id "microsoft.windows.sdk.cpp" -Version $version -Destination (Join-Path $winsdk "base")
    Get-NuGetPackage -Id "microsoft.windows.sdk.cpp.x64" -Version $version -Destination (Join-Path $winsdk "x64")
}

# --- Windows SDK build tools (rc.exe, mt.exe, signtool) ---------------------
$buildTools = Join-Path $winsdk "buildtools"
if ($Force -or -not (Get-ChildItem $buildTools -Recurse -Filter "rc.exe" -ErrorAction SilentlyContinue)) {
    Get-NuGetPackage -Id "microsoft.windows.sdk.buildtools" -Destination $buildTools
}

if (-not (Test-Path (Join-Path $webview2 "build\native\include\WebView2.h"))) {
    throw "WebView2 SDK extraction failed."
}
if (-not (Test-Path (Join-Path $winsdk "base\c\Include"))) {
    throw "Windows SDK headers extraction failed."
}
if (-not (Test-Path (Join-Path $winsdk "x64\c\um\x64\kernel32.lib"))) {
    throw "Windows SDK x64 libs extraction failed."
}

Write-Host ""
Write-Host "Dependencies ready:"
Write-Host "  WebView2: $webview2"
Write-Host "  WinSDK:   $winsdk"
