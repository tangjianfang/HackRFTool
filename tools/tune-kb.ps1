# Real keyboard tune: click edit, Ctrl+A, type digits, apply via WM_COMMAND.
# ID 对照 src/app/main.cpp 控件枚举（#93 修正：旧值 115/111 已漂移成
# IDC_EDIT_MON/IDC_CLEAR——旧脚本会点监测页频率框并误触「清空」）：
#   IDC_EDIT_FREQ=114（通用行中心频率框）  IDC_APPLYFREQ=112（应用按钮）
param([string]$Mhz = "107.1")
$ErrorActionPreference = "Stop"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Kb {
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc fn, IntPtr lp);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct R { public int Left, Top, Right, Bottom; }
}
"@
[Kb]::SetProcessDPIAware() | Out-Null
$p = Get-Process HackRFTool -ErrorAction Stop
$main = $p.MainWindowHandle
$script:hit = [IntPtr]::Zero
$cb = [Kb+EnumProc]{ param($h, $l)
  if ([Kb]::GetDlgCtrlID($h) -eq 114) { $script:hit = $h; return $false }
  return $true }
[Kb]::EnumChildWindows($main, $cb, [IntPtr]::Zero) | Out-Null
if ($script:hit -eq [IntPtr]::Zero) { Write-Error "edit not found"; exit 1 }
$r = New-Object Kb+R
[Kb]::GetWindowRect($script:hit, [ref]$r) | Out-Null
[Kb]::SetForegroundWindow($main) | Out-Null
Start-Sleep -Milliseconds 300
[Kb]::SetCursorPos([int](($r.Left+$r.Right)/2), [int](($r.Top+$r.Bottom)/2)) | Out-Null
Start-Sleep -Milliseconds 120
[Kb]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 50
[Kb]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 250
# Ctrl+A as proper chord: ctrl down, A down, A up, ctrl up
[Kb]::keybd_event(0x11,0,0,[UIntPtr]::Zero)
[Kb]::keybd_event(0x41,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 30
[Kb]::keybd_event(0x41,0,2,[UIntPtr]::Zero)
[Kb]::keybd_event(0x11,0,2,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 150
foreach ($c in $Mhz.ToCharArray()) {
  $vk = if ($c -ge '0' -and $c -le '9') { [byte]([int][char]$c - 48 + 0x30) }
        elseif ($c -eq '.') { [byte]0xBE } else { [byte]0 }
  if ($vk -eq 0) { continue }
  [Kb]::keybd_event($vk,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 30
  [Kb]::keybd_event($vk,0,2,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60
}
Start-Sleep -Milliseconds 200
[Kb]::SendMessage($main, 0x111, [IntPtr]112, [IntPtr]::Zero) | Out-Null
Write-Host "typed $Mhz + applied"
