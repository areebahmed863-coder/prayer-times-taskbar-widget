# Taskbar Widgets Lite — Prayer Times & Media

A **pure native C++** lightweight fork of [Taskbar Widgets](https://github.com/pfcdev/TaskbarWidgets) for Windows 11, stripped down to **Prayer Times** (built-in) and an optional **Media Player** widget, with layout fixes for **portrait and secondary monitors**.

```
Maghrib 5:42 PM
18m left
▬▬▬▬▬▬▬▬▬▬▬▬░░░     <- progress through the current prayer window
```

## What makes it lightweight?

Compared with full Taskbar Widgets (0.5.36):

| | Full Taskbar Widgets | Taskbar Widgets Lite |
|---|---|---|
| **Runtime stack** | 4x self-contained .NET 8 apps + Rust/Tauri + WebView2 + PowerShell | **Pure native C++ (Win32 + WinHTTP)** |
| **Installer size** | ~175 MB | **< 1 MB** |
| **Installed size** | ~285 MB | **~1.2 MB** |
| **Prayer Times provider** | Persistent `powershell.exe` (~50 MB RAM) | **Built-in native C++ thread (~3–5 MB total RAM)** |
| **Settings UI** | 15 MB Tauri/WebView2 app | **Native Win32 tray menu + settings dialog + `Data\config.json`** |
| **Media Player** | Always bundled with 9 other built-in widgets | **Optional 154 KB native helper — 0 MB RAM when disabled** |

## Install

1. Download `TaskbarWidgetsSetup-x64.exe` (or `TaskbarWidgets-portable-x64.zip`) from the [latest release](../../releases/latest).
2. Run it. Windows SmartScreen may warn because the installer is **unsigned**: choose *More info > Run anyway* only if you trust this repository.
3. Prayer Times starts immediately on the taskbar — no `.twidget` install or permission review needed.
4. Click the **Taskbar Widgets Lite** icon in the system tray to:
   - Open **Prayer Times Settings...** (City, Country, Calculation Method, Hanafi/Standard Asr, 12/24-hour clock)
   - Toggle **Prayer Times** or **Media Player** on/off
   - **Refresh Prayer Times** or open **`Data\config.json`** directly for advanced layout/visualizer tweaks
   - Toggle **Start with Windows** or **Exit**

The installer cleanly replaces an existing full Taskbar Widgets install and preserves `Data\config.json`.

## Prayer Times widget

- Next prayer, its time, countdown, and a progress bar for the current prayer window
- Click the widget on the taskbar to open a flyout with all daily prayer times, Sunrise, and the Hijri date
- Hanafi Asr by default (Standard is a setting), 12/24-hour clock
- Calculation methods: ISNA, Muslim World League, Umm al-Qura, Egyptian General Authority, Karachi, Moonsighting Committee
- Times come from the free [Aladhan API](https://aladhan.com/prayer-times-api) and are cached in `Data\Cache\PrayerTimes` for offline use
- 124 px wide so it fits narrow and portrait taskbars

## Media Player widget (optional)

- Disabled by default (0 MB RAM when off); enable anytime from the tray menu
- Native Windows media session (SMTC) with album art, play/pause/next/previous buttons, and a real-time WASAPI audio visualizer (`TaskbarWidgets.MediaHelper.exe`, ~154 KB)

## Portrait & secondary monitor fixes

Compared with upstream 0.5.36 (`src/native/taskbar-hook/taskbar_widgets_hook.cpp`):

1. Each taskbar gets its **own** icon positions so widgets on portrait/secondary monitors don't overlap app icons.
2. Narrow taskbars (under 1400 px) reserve only the real tray width instead of at least 220 px.
3. A widget that does not fit is not made visible again by every data update, preventing flashing on narrow taskbars.

## Build

Only **Visual Studio 2022 Build Tools (C++)**, **CMake**, and **NSIS** (for installer packaging) are required — no .NET SDK, Rust, or Node.js.

```powershell
.\build.ps1 -Target Verify
.\build.ps1 -Target Package
```

Outputs are written to `artifacts\TaskbarWidgetsSetup-x64.exe` and `artifacts\TaskbarWidgets-portable-x64.zip`.

## Credits and license

MIT. Based on Taskbar Widgets by PFC (https://github.com/pfcdev/TaskbarWidgets). The original README is kept as `UPSTREAM-README.md`. Prayer times data is from Aladhan.
