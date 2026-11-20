# Taskbar Widgets + Prayer Times

A build of [Taskbar Widgets](https://github.com/pfcdev/TaskbarWidgets) 0.5.36 for Windows 11 that
ships with a **Prayer Times** widget and fixes widget layout on **portrait and secondary monitors**.

```
Maghrib 5:42 PM
18m left
▬▬▬▬▬▬▬▬▬▬▬▬░░░     <- progress through the current prayer window
```

## Install

1. Download `TaskbarWidgetsSetup-x64.exe` from the [latest release](../../releases/latest).
2. Run it. Windows SmartScreen will warn because the installer is **unsigned**: choose
   *More info > Run anyway* only if you trust this repository.
3. On the last page leave **Add the Prayer Times widget** ticked. Taskbar Widgets Settings opens
   the normal permission review for the widget. Read it, then approve.
4. Enable the widget, then open its page in Settings and set your **City**, **Country**,
   **Calculation method** and **Asr calculation** (Hanafi is the default).

The widget can also be installed later: double-click
`%LOCALAPPDATA%\Programs\TaskbarWidgets\Bundled\com.areeb.prayer-times.twidget`.

The installer replaces an existing Taskbar Widgets install and keeps its settings.

## Prayer Times widget

- Next prayer, its time, time left, and a progress bar for the current prayer window
- Hanafi Asr by default (Standard is a setting), 12/24-hour clock
- Calculation methods: ISNA, Muslim World League, Umm al-Qura, Egyptian, Karachi, Moonsighting
- Times come from the free [Aladhan API](https://aladhan.com/prayer-times-api) and are cached for offline use
- 124 px wide so it fits narrow taskbars
- Permissions it asks for: full access (it runs a PowerShell script as you), internet access to
  `api.aladhan.com`, a cache folder in `%LOCALAPPDATA%\TaskbarWidgets\PrayerTimes`, and background running
- Source: [`bundled-widgets/com.areeb.prayer-times`](bundled-widgets/com.areeb.prayer-times)

## What changed in the host

Compared with upstream 0.5.36 (`src/native/taskbar-hook/taskbar_widgets_hook.cpp`):

1. Each taskbar now gets its **own** icon positions. Before, every taskbar used the first one's, so
   widgets on a portrait taskbar overlapped the app icons.
2. Narrow taskbars (under 1400 px) reserve only the real tray width instead of at least 220 px.
3. A widget that does not fit is no longer made visible again by every data update, which made it
   flash on narrow taskbars.

Other changes: `bundled-widgets/` is packed into the installer, the installer's finish page offers
the widget, and the updater and release script point at this repository.

Known limitation: a widget wider than the free space on a narrow taskbar is hidden rather than
squeezed (for example the 220 px Media Player on a 1080 px portrait taskbar).

## Build

Requirements are the same as upstream (see `docs/building.md`): Windows 11, Visual Studio 2022
Build Tools (C++), CMake, .NET 8 SDK, Rust, NSIS.

```powershell
.\build.ps1 -Target Package
```

The installer is written to `artifacts\TaskbarWidgetsSetup-x64.exe`.

## Credits and license

MIT. Taskbar Widgets is by PFC (https://github.com/pfcdev/TaskbarWidgets); this repository modifies
their source. The original README is kept as `UPSTREAM-README.md`. Prayer times data is from Aladhan.
