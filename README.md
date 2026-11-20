# Prayer Times for Taskbar Widgets

A prayer times widget for [Taskbar Widgets](https://github.com/pfcdev/TaskbarWidgets)
(v0.5.36) on Windows 11, plus a small host patch that makes widgets behave on
**portrait and secondary monitors**.

```
Maghrib 5:42 PM
43m left
▬▬▬▬▬▬▬▬░░░░░░     <- progress through the current prayer window
```

- Next prayer, its time, time left, and a progress bar
- Hanafi Asr by default (switchable), 12/24-hour clock
- Times from the free [Aladhan API](https://aladhan.com/prayer-times-api), cached for offline use
- 124 px wide so it fits narrow taskbars

## Install the widget

1. Install Taskbar Widgets 0.5.36 from the upstream project.
2. Download `com.areeb.prayer-times.twidget` from the latest release of this repo.
3. Open Taskbar Widgets **Settings > Developer** (or the install/import option), choose the
   `.twidget` and review the permissions. It runs a PowerShell script with full user rights,
   fetches `api.aladhan.com`, and writes a cache to `%LOCALAPPDATA%\TaskbarWidgets\PrayerTimes`.
4. Enable the widget, then open its page in Settings and set your **City**, **Country**,
   **Calculation method** and **Asr calculation** (Standard or Hanafi).

You can also open the `widget/com.areeb.prayer-times` folder directly from Settings > Developer.

## Portrait / secondary monitor patch (optional)

Stock 0.5.36 has three problems on a taskbar that is not the primary landscape one:

1. Every taskbar was given the icon positions of the *same* taskbar, so widgets on a portrait
   taskbar overlapped the app icons.
2. A fixed 220 px area was reserved for the tray, wasting space on narrow taskbars.
3. A widget that does not fit was hidden by the layout and shown again by every data update,
   so it flashed.

`host-patch/portrait-fixes.patch` is the source diff against upstream commit `c49721c`.
`host-patch/TaskbarWidgets.Hook.dll` is a prebuilt, **unsigned** build of the patched hook
(SHA-256 `796818A43901A5BC3B8F4556C94924857693C74CAB97E3F99FBD908518C9F648`).

```powershell
cd host-patch
.\install-host-patch.ps1            # backs up the stock DLL, restarts Explorer, installs the patch
.\install-host-patch.ps1 -Restore   # put the stock DLL back
```

The script refuses to run unless your installed hook is the stock 0.5.36 build. This DLL runs
**inside Explorer**: only use it if you are comfortable with that, or build it yourself from the
patch. If Taskbar Widgets updates itself it may restore its own hook.

Known limitation: a widget wider than the free space on a narrow taskbar is hidden rather than
squeezed (for example the 220 px Media Player widget on a 1080 px portrait taskbar).

## Build the hook yourself

Apply `portrait-fixes.patch` to `src/native/taskbar-hook/taskbar_widgets_hook.cpp` in the upstream
repo and build it with MSVC (`/MD /O1 /Os`). The loader only rewrites the extracted DLL when its
size differs from the embedded one, so pad the output with zeros to the stock size (769,536 bytes).

## Credits and license

MIT. Taskbar Widgets is by PFC (https://github.com/pfcdev/TaskbarWidgets, MIT); the host patch is a
modification of their source. Prayer times data comes from Aladhan.
