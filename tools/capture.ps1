# Captures the MooClient window to a PNG for manual verification.
param(
    [string]$Out = "$env:TEMP\MooClient-window.png"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MooClientCapture {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, IntPtr windowName);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hwnd, int command);
}
"@

$hwnd = [MooClientCapture]::FindWindow("MooClientMainWindow", [IntPtr]::Zero)
if ($hwnd -eq [IntPtr]::Zero) { throw "MooClient window not found." }

[void][MooClientCapture]::ShowWindow($hwnd, 9)      # SW_RESTORE
[void][MooClientCapture]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 400

$rect = New-Object MooClientCapture+RECT
if (-not [MooClientCapture]::GetWindowRect($hwnd, [ref]$rect)) { throw "GetWindowRect failed." }

$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) { throw "Window has no area." }

$bitmap = New-Object System.Drawing.Bitmap($width, $height)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose()
$bitmap.Dispose()

Write-Host $Out
