# Starts MooClient, waits for the game to settle, captures a screenshot and prints
# the log tail. Useful for repeatable M0 acceptance runs.
param(
    [int]$WaitSeconds = 10,
    [switch]$Stop
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\MooClient.exe"
if (-not (Test-Path $exe)) { throw "Build first: $exe is missing." }

$existing = Get-Process MooClient -ErrorAction SilentlyContinue
if (-not $existing) {
    Start-Process -FilePath $exe | Out-Null
    Write-Host "Started MooClient."
} else {
    Write-Host "MooClient already running (pid $($existing.Id))."
}

Start-Sleep -Seconds $WaitSeconds
& (Join-Path $PSScriptRoot "capture.ps1")

Write-Host ""
Write-Host "--- client.log (tail) ---"
$log = Join-Path $env:LOCALAPPDATA "MooClient\logs\client.log"
if (Test-Path $log) {
    try { Get-Content $log -Tail 40 }
    catch { Write-Host "(log is currently locked by the app)" }
} else {
    Write-Host "(no log yet)"
}

if ($Stop) {
    Get-Process MooClient -ErrorAction SilentlyContinue | Stop-Process -Force
    Write-Host "Stopped MooClient."
}
