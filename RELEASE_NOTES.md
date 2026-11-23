## Taskbar Widgets Lite (v0.5.37)

Rebuilt as a **pure native C++** lightweight version of Taskbar Widgets focused on **Prayer Times** and an optional **Media Player**:

- **~337x smaller installer**: `TaskbarWidgetsSetup-x64.exe` dropped from ~175 MB to **~531 KB** (no .NET 8 runtimes, no Rust/Tauri, no WebView2 bootstrapper).
- **Native Prayer Times provider**: Built directly into `TaskbarWidgets.exe` using WinHTTP + offline daily caching (replaces the persistent PowerShell process) with a click flyout showing all daily prayer times and the Hijri date.
- **Native tray menu & settings**: Configure City, Country, Calculation Method, Hanafi/Standard Asr, and 12/24-hour clock directly from the system tray icon or edit `Data\config.json`.
- **Optional Media Player**: Keeps the ~154 KB native `TaskbarWidgets.MediaHelper.exe` (disabled by default, 0 MB RAM when off; toggleable from the tray menu).
- **Keeps portrait & secondary monitor layout fixes** from v0.5.36.

`TaskbarWidgetsSetup-x64.exe` supports both fresh installations and in-place upgrades from full Taskbar Widgets while preserving `Data\config.json`.
