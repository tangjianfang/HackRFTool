param([string]$Title = "HackRFTool", [string]$Out = "shot.png")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Shot {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$h = [Win32Shot]::FindWindow($null, $Title)
if ($h -eq [IntPtr]::Zero) { Write-Error "未找到窗口: $Title"; exit 1 }
[Win32Shot]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500
$r = New-Object Win32Shot+RECT
[Win32Shot]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "已保存 $Out (${w}x${ht})"
