# Foreground screen capture (BitBlt) — unlike PrintWindow, shows ALL children.
param([string]$Proc = "HackRFTool", [string]$Out = "shot.png", [int]$WaitSec = 10)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Fg {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  public struct R { public int Left, Top, Right, Bottom; }
}
"@
[Fg]::SetProcessDPIAware() | Out-Null
$h = [IntPtr]::Zero
for ($i = 0; $i -lt $WaitSec * 2; $i++) {
    $p = Get-Process $Proc -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne 0) { $h = $p.MainWindowHandle; break }
    $h = [Fg]::FindWindow($null, $Proc)
    if ($h -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
}
if ($h -eq [IntPtr]::Zero) { Write-Error "window not found"; exit 1 }
[Fg]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
[Fg]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
[Fg]::ShowWindow($h, 9) | Out-Null
[Fg]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 800
$r = New-Object Fg+R
[Fg]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved $Out (${w}x${ht})"
