# Capture a window by handle via PrintWindow (works even when occluded).
# Activates the window first: the render loop may stall while fully occluded.
# ASCII-only on purpose: PS 5.1 reads BOM-less files as ANSI, so non-ASCII
# comments can silently corrupt parsing.
param([string]$Proc = "HackRFTool", [string]$Out = "shot.png", [int]$WaitSec = 10)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Cap {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[Win32Cap]::SetProcessDPIAware() | Out-Null

$h = [IntPtr]::Zero
for ($i = 0; $i -lt $WaitSec * 2; $i++) {
    $p = Get-Process $Proc -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne 0) { $h = $p.MainWindowHandle; break }
    $h = [Win32Cap]::FindWindow($null, $Proc)
    if ($h -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
}
if ($h -eq [IntPtr]::Zero) { Write-Error "window not found: $Proc"; exit 1 }

# Simulate ALT press/release to bypass the foreground lock, then activate.
[Win32Cap]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
[Win32Cap]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
[Win32Cap]::ShowWindow($h, 9) | Out-Null
[Win32Cap]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 800

$r = New-Object Win32Cap+RECT
[Win32Cap]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Win32Cap]::PrintWindow($h, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved $Out (${w}x${ht})"
