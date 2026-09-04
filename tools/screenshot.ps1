# 按窗口句柄截图（PrintWindow）：被其它窗口遮挡也能截，不影响用户桌面
param([string]$Proc = "HackRFTool", [string]$Out = "shot.png", [int]$WaitSec = 10)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Cap {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[Win32Cap]::SetProcessDPIAware() | Out-Null   # 取物理像素尺寸，避免 DPI 虚拟化裁剪

$h = [IntPtr]::Zero
for ($i = 0; $i -lt $WaitSec * 2; $i++) {
    $p = Get-Process $Proc -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne 0) { $h = $p.MainWindowHandle; break }
    Start-Sleep -Milliseconds 500
}
if ($h -eq [IntPtr]::Zero) { Write-Error "未找到窗口: $Proc"; exit 1 }

$r = New-Object Win32Cap+RECT
[Win32Cap]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Win32Cap]::PrintWindow($h, $hdc, 2) | Out-Null   # 2 = PW_RENDERFULLCONTENT（DComp 内容可截）
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "已保存 $Out (${w}x${ht})"
