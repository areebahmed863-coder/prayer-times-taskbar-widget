# Installs the portrait-monitor host patch into Taskbar Widgets 0.5.36.
# It only proceeds if the installed hook DLL is the exact stock 0.5.36 build.
# Run from PowerShell:  .\install-host-patch.ps1      (add -Restore to undo)
param([switch]$Restore)

$ErrorActionPreference = "Stop"
$stockSha256 = "8FF2623C0768B5604B121ABA81F4270394A46DF6F4949CA99661BAF9715AD46F"
$install = Join-Path $env:LOCALAPPDATA "Programs\TaskbarWidgets"
$runtime = Join-Path $install "Data\Runtime"
$patched = Join-Path $PSScriptRoot "TaskbarWidgets.Hook.dll"
$backup  = Join-Path $install "Data\TaskbarWidgets.Hook.dll.stock-0.5.36"

if (-not (Test-Path "$install\TaskbarWidgets.exe")) { throw "Taskbar Widgets is not installed at $install" }
$target = Get-ChildItem $runtime -Recurse -Filter TaskbarWidgets.Hook.dll | Select-Object -First 1
if (-not $target) { throw "Start Taskbar Widgets once so it extracts its hook, then run this again." }

if ($Restore) {
    if (-not (Test-Path $backup)) { throw "No backup found at $backup" }
    $source = $backup
} else {
    $current = (Get-FileHash $target.FullName -Algorithm SHA256).Hash
    if ($current -ne $stockSha256 -and -not (Test-Path $backup)) {
        throw "Installed hook is not the stock 0.5.36 build (SHA-256 $current). Not touching it."
    }
    if ($current -eq $stockSha256) { Copy-Item $target.FullName $backup -Force }
    $source = $patched
}

Write-Host "Stopping Taskbar Widgets and restarting Explorer (the taskbar will disappear for a few seconds)..."
Get-Process TaskbarWidgets, TaskbarWidgets.MediaHelper -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
Get-Process explorer | Stop-Process -Force
for ($i = 0; $i -lt 20 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
Start-Sleep 4
Copy-Item $source $target.FullName -Force
Start-Process "$install\TaskbarWidgets.exe" -WorkingDirectory $install
Write-Host ($(if ($Restore) { "Stock hook restored." } else { "Patched hook installed. Stock copy kept at $backup" }))
