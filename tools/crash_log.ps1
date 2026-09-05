# Dump recent Application Error (1000) events for HackRFTool (ASCII only)
param([int]$Max = 8)
$events = Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000} -MaxEvents 40 -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match 'HackRFTool' } | Select-Object -First $Max
if (-not $events) { Write-Host "no HackRFTool crash events found"; exit 0 }
foreach ($e in $events) {
    Write-Host ("=== " + $e.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss') + " ===")
    $lines = $e.Message -split "`r?`n"
    $lines | Select-Object -First 12 | ForEach-Object { Write-Host $_ }
}
