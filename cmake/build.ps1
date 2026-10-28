# Configures and builds MooClient with MSVC + Ninja.
param(
    [string]$Config = "RelWithDebInfo",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"

$vcvarsCandidates = @(
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
)

$vcvars = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw "vcvars64.bat not found. Install Visual Studio Build Tools with the C++ workload." }

# pip installs cmake/ninja into the per-user Scripts directory, which is often
# not on PATH. Add the usual locations before giving up.
foreach ($dir in @(
    (Join-Path $env:APPDATA "Python\Python315\Scripts"),
    (Join-Path $env:APPDATA "Python\Python314\Scripts"),
    "C:\Python315\Scripts",
    "C:\Python314\Scripts"
)) {
    if (Test-Path $dir) { $env:PATH = "$dir;$env:PATH" }
}

foreach ($tool in @("cmake", "ninja")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool not found. Run: python -m pip install cmake ninja"
    }
}

if ($Clean -and (Test-Path $build)) {
    Remove-Item $build -Recurse -Force
}

# CMake's compiler probe invokes rc.exe to build a test manifest, so the SDK
# build tools must be on PATH before the first configure.
$buildTools = Join-Path $root "third_party\winsdk\buildtools"
$rc = Get-ChildItem $buildTools -Recurse -Filter "rc.exe" -ErrorAction SilentlyContinue |
      Where-Object { $_.FullName -match "\\x64\\" } | Select-Object -First 1
$mt = Get-ChildItem $buildTools -Recurse -Filter "mt.exe" -ErrorAction SilentlyContinue |
      Where-Object { $_.FullName -match "\\x64\\" } | Select-Object -First 1
if ($rc) { $env:PATH = "$($rc.DirectoryName);$env:PATH" }
if ($mt) { $env:PATH = "$($mt.DirectoryName);$env:PATH" }

# Import the MSVC environment into this process.
$envText = cmd /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed." }
foreach ($line in $envText) {
    $idx = $line.IndexOf('=')
    if ($idx -le 0) { continue }
    $name = $line.Substring(0, $idx)
    if ($name.StartsWith('=')) { continue }
    Set-Item -Path "Env:$name" -Value $line.Substring($idx + 1)
}

# Windows SDK headers/libs assembled from Microsoft's NuGet packages (no admin).
# If a real Windows SDK is installed, INCLUDE/LIB already point at it and this is
# harmless duplication.
$sdk = Join-Path $root "third_party\winsdk"
$sdkIncludeRoot = Join-Path $sdk "base\c\Include"
if (Test-Path $sdkIncludeRoot) {
    $sdkVersion = Get-ChildItem $sdkIncludeRoot -Directory | Select-Object -First 1
    $includeDirs = @(
        (Join-Path $sdkVersion.FullName "um"),
        (Join-Path $sdkVersion.FullName "shared"),
        (Join-Path $sdkVersion.FullName "ucrt"),
        (Join-Path $sdkVersion.FullName "winrt")
    ) | Where-Object { Test-Path $_ }

    $existingInclude = $env:INCLUDE
    $env:INCLUDE = $includeDirs -join ';'
    if ($existingInclude) { $env:INCLUDE += ";$existingInclude" }

    $libDirs = @(
        (Join-Path $sdk "x64\c\um\x64"),
        (Join-Path $sdk "x64\c\ucrt\x64")
    ) | Where-Object { Test-Path $_ }

    $existingLib = $env:LIB
    $env:LIB = $libDirs -join ';'
    if ($existingLib) { $env:LIB += ";$existingLib" }
}

$cmakeArgs = @("-S", $root, "-B", $build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config")
if ($rc) { $cmakeArgs += "-DCMAKE_RC_COMPILER=$($rc.FullName -replace '\\','/')" }
if ($mt) { $cmakeArgs += "-DCMAKE_MT=$($mt.FullName -replace '\\','/')" }

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }

& cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

$exe = Join-Path $build "bin\MooClient.exe"
Write-Host ""
Write-Host "Built: $exe"
