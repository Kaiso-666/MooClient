# Downloads the WebView2 Fixed Version runtime (x64) into third_party/webview2-fixed.
#
# A fixed runtime makes MooClient independent of the WebView2 runtime installed on
# the machine and immune to Edge updates. It is large (~300 MB cab, ~600 MB
# extracted), so it is optional for development and only required for packaging.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File cmake/fetch_webview2_runtime.ps1
#   powershell -ExecutionPolicy Bypass -File cmake/fetch_webview2_runtime.ps1 -Deploy
#
# -Deploy copies the extracted runtime to build/bin/runtime, where MooClient
# detects it automatically (no --runtime argument needed).
param(
    [string]$Version = "",
    [switch]$Deploy,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\webview2-fixed"

function Resolve-FixedUrl {
    param([string]$WantedVersion)

    Write-Host "Resolving WebView2 Fixed Version download ..."
    $html = (Invoke-WebRequest -Uri "https://developer.microsoft.com/en-us/microsoft-edge/webview2/" `
                               -UseBasicParsing).Content
    # The page stores URLs with JSON-escaped slashes.
    $html = $html -replace '\\u002F', '/'

    $pattern = 'https://msedge[^"''\s]*?FixedVersionRuntime\.([0-9.]+)\.x64\.cab'
    $matches = [regex]::Matches($html, $pattern)
    if ($matches.Count -eq 0) { throw "Could not find a fixed-version x64 cab on the WebView2 page." }

    if ($WantedVersion) {
        foreach ($m in $matches) {
            if ($m.Groups[1].Value -eq $WantedVersion) {
                return @{ Url = $m.Value; Version = $WantedVersion }
            }
        }
        throw "Fixed version $WantedVersion not listed on the page."
    }

    $first = $matches[0]
    return @{ Url = $first.Value; Version = $first.Groups[1].Value }
}

$resolved = Resolve-FixedUrl -WantedVersion $Version
$version = $resolved.Version
$target = Join-Path $dest $version

if ((Test-Path (Join-Path $target "msedgewebview2.exe")) -and -not $Force) {
    Write-Host "WebView2 Fixed Version $version already extracted at $target"
} else {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    $cab = Join-Path $dest "$version.cab"

    if (-not (Test-Path $cab) -or $Force) {
        Write-Host "Downloading $version ..."
        curl.exe -L --retry 5 --retry-delay 2 -o $cab $resolved.Url
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $($resolved.Url)" }
    }

    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $target | Out-Null

    Write-Host "Extracting ..."
    expand.exe -F:* $cab $target | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "expand.exe failed with exit code $LASTEXITCODE" }

    # The cab may contain a single top-level folder; flatten it if so.
    $exe = Get-ChildItem $target -Recurse -Filter "msedgewebview2.exe" |
           Select-Object -First 1
    if (-not $exe) { throw "Extraction failed: msedgewebview2.exe not found." }
    if ($exe.DirectoryName -ne $target) {
        Get-ChildItem $exe.DirectoryName -Force | Move-Item -Destination $target -Force
    }
}

$runtimeExe = Join-Path $target "msedgewebview2.exe"
if (-not (Test-Path $runtimeExe)) { throw "Runtime is incomplete: $runtimeExe is missing." }

Write-Host "WebView2 Fixed Version $version ready at $target"

if ($Deploy) {
    $bin = Join-Path $root "build\bin"
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    $runtime = Join-Path $bin "runtime"
    if (Test-Path $runtime) { Remove-Item $runtime -Recurse -Force }
    Copy-Item $target $runtime -Recurse -Force
    Write-Host "Deployed to $runtime"
}
