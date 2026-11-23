#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "prayer_provider.h"
#include "resource.h"

namespace fs = std::filesystem;
namespace wdj = winrt::Windows::Data::Json;

namespace {

constexpr wchar_t kAppName[] = L"TaskbarWidgets";
constexpr wchar_t kMutexName[] = L"Local\\TaskbarWidgetsProductLoader";
constexpr wchar_t kLoaderShutdownEventName[] =
    L"Local\\TaskbarWidgetsLoaderShutdown";
constexpr wchar_t kTrayWindowClass[] = L"TaskbarWidgetsLiteTrayWindow";
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_OPEN_SETTINGS = WM_APP + 2;

enum TrayMenuId : UINT {
    IDM_TOGGLE_WIDGETS = 4001,
    IDM_TOGGLE_PRAYER = 4002,
    IDM_TOGGLE_MEDIA = 4003,
    IDM_PRAYER_SETTINGS = 4004,
    IDM_REFRESH_PRAYER = 4005,
    IDM_OPEN_CONFIG = 4006,
    IDM_TOGGLE_STARTUP = 4007,
    IDM_EXIT = 4008,
};

struct MethodOption {
    const wchar_t* value;
    const wchar_t* label;
};

constexpr MethodOption kMethods[] = {
    {L"2", L"ISNA (North America)"},
    {L"3", L"Muslim World League"},
    {L"4", L"Umm al-Qura (Makkah)"},
    {L"5", L"Egyptian General Authority"},
    {L"1", L"Univ. of Islamic Sciences, Karachi"},
    {L"15", L"Moonsighting Committee"},
};

constexpr MethodOption kSchools[] = {
    {L"1", L"Hanafi"},
    {L"0", L"Standard (Shafi, Maliki, Hanbali)"},
};

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                  static_cast<int>(value.size()), nullptr, 0,
                                  nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                  static_cast<int>(value.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), out.data(), len);
    return out;
}

bool AtomicWriteUtf8(const std::wstring& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::wstring tmpPath = path + L".tmp";
    HANDLE file = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(file, content.data(),
                        static_cast<DWORD>(content.size()), &written, nullptr);
    if (ok && written == content.size()) {
        FlushFileBuffers(file);
    } else {
        ok = FALSE;
    }
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    if (!MoveFileExW(tmpPath.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

std::string ReadFileUtf8(const std::wstring& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::wstring GetExePath() {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return std::wstring(buf, len);
}

std::wstring GetInstallDir() {
    return fs::path(GetExePath()).parent_path().wstring();
}

std::wstring ComputeFileSha256Prefix16(const std::wstring& path) {
    std::string bytes = ReadFileUtf8(path);
    if (bytes.empty()) return L"default";

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::wstring result = L"default";
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(
                    hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                    static_cast<ULONG>(bytes.size()), 0) == 0) {
                UCHAR digest[32]{};
                if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                    wchar_t hex[17]{};
                    for (int i = 0; i < 8; ++i) {
                        swprintf_s(hex + i * 2, 3, L"%02x", digest[i]);
                    }
                    result = hex;
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return result;
}

struct AppState {
    std::wstring installDir;
    std::wstring dataDir;
    std::wstring stateDir;
    std::wstring commandsDir;
    std::wstring logsDir;
    std::wstring runtimeDir;
    std::wstring configPath;
    std::wstring runtimeControlPath;
    std::wstring hookRuntimePath;
    std::wstring mediaHelperPath;

    std::mutex logMutex;
    std::mutex configMutex;
    bool consoleEnabled = false;

    std::atomic_bool stopRequested{false};
    std::atomic_bool widgetsEnabled{true};
    std::atomic_bool prayerEnabled{true};
    std::atomic_bool mediaEnabled{false};
    PrayerSettings prayerSettings;

    HWND trayHwnd = nullptr;
    UINT taskbarCreatedMsg = 0;
    NOTIFYICONDATAW nid{};
    bool settingsDialogOpen = false;

    std::unique_ptr<PrayerProvider> prayerProvider;

    void Log(const std::wstring& message) {
        std::lock_guard lock(logMutex);
        try {
            std::error_code ec;
            fs::create_directories(logsDir, ec);
            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t prefix[64]{};
            swprintf_s(prefix, L"%04u-%02u-%02uT%02u:%02u:%02u.%03u [loader] ",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds);
            std::wstring line = std::wstring(prefix) + message + L"\r\n";
            std::ofstream out(fs::path(logsDir + L"\\loader.log"),
                              std::ios::binary | std::ios::app);
            if (out) {
                std::string utf8 = WideToUtf8(line);
                out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
            }
            if (consoleEnabled) {
                fwprintf(stderr, L"%s", line.c_str());
            }
        } catch (...) {
        }
    }
};

AppState* g_app = nullptr;

// ---------- Startup registry helpers ----------

bool IsStartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    bool exists = RegQueryValueExW(key, kAppName, nullptr, &type, nullptr,
                                   &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

void InstallStartup() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                        nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) == ERROR_SUCCESS) {
        std::wstring value = L"\"" + GetExePath() + L"\"";
        RegSetValueExW(key, kAppName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

void UninstallStartup() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kAppName);
        RegCloseKey(key);
    }
}

// ---------- Explorer hook injection / detach ----------

std::vector<DWORD> GetCurrentSessionExplorers() {
    std::vector<DWORD> pids;
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"explorer.exe") == 0) {
                DWORD sess = 0;
                if (ProcessIdToSessionId(entry.th32ProcessID, &sess) &&
                    sess == mySession) {
                    pids.push_back(entry.th32ProcessID);
                }
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pids;
}

bool IsHookLoadedInProcess(DWORD pid, const std::wstring& hookPath) {
    HANDLE proc =
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return false;

    HMODULE modules[1024]{};
    DWORD needed = 0;
    bool loaded = false;
    if (EnumProcessModulesEx(proc, modules, sizeof(modules), &needed,
                             LIST_MODULES_ALL)) {
        DWORD count = std::min<DWORD>(needed / sizeof(HMODULE),
                                      static_cast<DWORD>(ARRAYSIZE(modules)));
        for (DWORD i = 0; i < count; ++i) {
            wchar_t path[MAX_PATH]{};
            if (GetModuleFileNameExW(proc, modules[i], path, ARRAYSIZE(path))) {
                if (_wcsicmp(path, hookPath.c_str()) == 0) {
                    loaded = true;
                    break;
                }
            }
        }
    }
    CloseHandle(proc);
    return loaded;
}

bool SignalHookEvent(const std::wstring& prefix, DWORD pid) {
    std::wstring name = prefix + std::to_wstring(pid);
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (!ev) return false;
    BOOL ok = SetEvent(ev);
    CloseHandle(ev);
    return ok != FALSE;
}

void InjectHookIntoProcess(DWORD pid, const std::wstring& hookPath) {
    if (g_app) g_app->Log(L"Injecting hook into explorer " + std::to_wstring(pid));
    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!proc) return;

    size_t byteLen = (hookPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(proc, nullptr, byteLen,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteMem) {
        if (WriteProcessMemory(proc, remoteMem, hookPath.c_str(), byteLen,
                               nullptr)) {
            HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
            FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");
            if (loadLib) {
                HANDLE thread = CreateRemoteThread(
                    proc, nullptr, 0,
                    reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib),
                    remoteMem, 0, nullptr);
                if (thread) {
                    WaitForSingleObject(thread, 10000);
                    CloseHandle(thread);
                }
            }
        }
        VirtualFreeEx(proc, remoteMem, 0, MEM_RELEASE);
    }
    CloseHandle(proc);
}

void DetachFromAllExplorers() {
    for (DWORD pid : GetCurrentSessionExplorers()) {
        SignalHookEvent(L"Local\\TaskbarWidgetsHookShutdown_", pid);
    }
}

void RefreshExplorerShell() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void SignalLoaderShutdown() {
    HANDLE ev =
        OpenEventW(EVENT_MODIFY_STATE, FALSE, kLoaderShutdownEventName);
    if (ev) {
        SetEvent(ev);
        CloseHandle(ev);
    }
}

void WaitForProcessesToExit(const wchar_t* exeName, DWORD timeoutMs) {
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    DWORD myPid = GetCurrentProcessId();
    while (GetTickCount64() < deadline) {
        bool found = false;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{sizeof(entry)};
            if (Process32FirstW(snap, &entry)) {
                do {
                    if (entry.th32ProcessID != myPid &&
                        _wcsicmp(entry.szExeFile, exeName) == 0) {
                        found = true;
                        break;
                    }
                } while (Process32NextW(snap, &entry));
            }
            CloseHandle(snap);
        }
        if (!found) return;
        Sleep(150);
    }
}

// ---------- Hook runtime deployment & WidgetCatalog ----------

std::wstring DeployHookDll(AppState& app) {
    std::wstring src = app.installDir + L"\\TaskbarWidgets.Hook.dll";
    std::wstring hash = ComputeFileSha256Prefix16(src);
    std::wstring targetDir = app.runtimeDir + L"\\" + hash;
    std::wstring target = targetDir + L"\\TaskbarWidgets.Hook.dll";

    std::error_code ec;
    fs::create_directories(targetDir, ec);
    if (!fs::exists(target, ec) ||
        fs::file_size(target, ec) != fs::file_size(src, ec)) {
        fs::copy_file(src, target, fs::copy_options::overwrite_existing, ec);
    }

    // Clean up stale hash directories
    for (const auto& entry : fs::directory_iterator(app.runtimeDir, ec)) {
        if (entry.is_directory() &&
            _wcsicmp(entry.path().filename().c_str(), hash.c_str()) != 0) {
            fs::remove_all(entry.path(), ec);
        }
    }
    return target;
}

void WriteWidgetCatalog(const AppState& app) {
    // Built-in Prayer Times declarative native layout (124x40 compact + click flyout)
    static const char* kCatalogJson = R"JSON({
  "schemaVersion": 1,
  "widgets": [
    {
      "id": "com.areeb.prayer-times",
      "displayName": "Prayer Times",
      "renderer": "native",
      "valid": true,
      "width": 124,
      "height": 40,
      "nativeRenderer": {
        "expandedWidth": 236,
        "expandedHeight": 206,
        "expandDirection": "auto",
        "activation": "click"
      },
      "layout": {
        "type": "card",
        "width": 124,
        "height": 40,
        "padding": { "left": 6, "top": 3, "right": 6, "bottom": 3 },
        "radius": 8,
        "backgroundColor": "#00000000",
        "children": [
          {
            "type": "column",
            "gap": 0,
            "children": [
              {
                "type": "text",
                "bind": "data.line1",
                "width": 112,
                "height": 16,
                "fontSize": 12,
                "fontWeight": "semibold",
                "trimming": "characterEllipsis",
                "color": "#FFFFFFFF"
              },
              {
                "type": "text",
                "bind": "data.line2",
                "width": 112,
                "height": 15,
                "fontSize": 11,
                "trimming": "characterEllipsis",
                "color": { "bind": "data.accent", "fallback": "#FF34D399" }
              },
              { "type": "spacer", "width": 1, "height": 2 },
              {
                "type": "progress",
                "value": { "bind": "data.progress" },
                "width": 112,
                "height": 3,
                "color": { "bind": "data.accent", "fallback": "#FF34D399" }
              }
            ]
          }
        ]
      },
      "expandedLayout": {
        "type": "column",
        "gap": 4,
        "children": [
          {
            "type": "row",
            "gap": 6,
            "children": [
              { "type": "text", "bind": "data.location", "width": 146, "height": 18, "fontSize": 13, "fontWeight": "semibold", "color": "#FFFFFFFF" },
              { "type": "button", "label": "Refresh", "action": "refresh", "width": 58, "height": 20 }
            ]
          },
          { "type": "text", "bind": "data.dateText", "width": 210, "height": 15, "fontSize": 10, "color": "#FF94A3B8" },
          { "type": "divider", "width": 210, "height": 1 },
          { "type": "row", "gap": 4, "children": [
            { "type": "text", "text": "Fajr", "width": 110, "height": 17, "fontSize": 12, "color": { "bind": "data.fajrColor", "fallback": "#FFE2E8F0" } },
            { "type": "text", "bind": "data.fajrTime", "width": 96, "height": 17, "fontSize": 12, "color": { "bind": "data.fajrColor", "fallback": "#FFE2E8F0" } }
          ]},
          { "type": "row", "gap": 4, "children": [
            { "type": "text", "text": "Sunrise", "width": 110, "height": 17, "fontSize": 12, "color": { "bind": "data.sunriseColor", "fallback": "#FF94A3B8" } },
            { "type": "text", "bind": "data.sunriseTime", "width": 96, "height": 17, "fontSize": 12, "color": { "bind": "data.sunriseColor", "fallback": "#FF94A3B8" } }
          ]},
          { "type": "row", "gap": 4, "children": [
            { "type": "text", "text": "Dhuhr", "width": 110, "height": 17, "fontSize": 12, "color": { "bind": "data.dhuhrColor", "fallback": "#FFE2E8F0" } },
            { "type": "text", "bind": "data.dhuhrTime", "width": 96, "height": 17, "fontSize": 12, "color": { "bind": "data.dhuhrColor", "fallback": "#FFE2E8F0" } }
          ]},
          { "type": "row", "gap": 4, "children": [
            { "type": "text", "text": "Asr", "width": 110, "height": 17, "fontSize": 12, "color": { "bind": "data.asrColor", "fallback": "#FFE2E8F0" } },
            { "type": "text", "bind": "data.asrTime", "width": 96, "height": 17, "fontSize": 12, "color": { "bind": "data.asrColor", "fallback": "#FFE2E8F0" } }
          ]},
          { "type": "row", "gap": 4, "children": [
            { "type": "text", "text": "Maghrib", "width": 110, "height": 17, "fontSize": 12, "color": { "bind": "data.maghribColor", "fallback": "#FFE2E8F0" } },
            { "type": "text", "bind": "data.maghribTime", "width": 96, "height": 17, "fontSize": 12, "color": { "bind": "data.maghribColor", "fallback": "#FFE2E8F0" } }
          ]},
          { "type": "row", "gap": 4, "children": [
            { "type": "text", "text": "Isha", "width": 110, "height": 17, "fontSize": 12, "color": { "bind": "data.ishaColor", "fallback": "#FFE2E8F0" } },
            { "type": "text", "bind": "data.ishaTime", "width": 96, "height": 17, "fontSize": 12, "color": { "bind": "data.ishaColor", "fallback": "#FFE2E8F0" } }
          ]}
        ]
      }
    }
  ]
}
)JSON";
    AtomicWriteUtf8(app.runtimeDir + L"\\WidgetCatalog.json", kCatalogJson);
}

// ---------- Config management (Data\config.json & runtime-control.json) ----------

wdj::JsonObject MakeDefaultPrayerWidgetJson() {
    wdj::JsonObject pos;
    pos.Insert(L"anchorPercent", wdj::JsonValue::CreateNumberValue(100));
    pos.Insert(L"offsetPx", wdj::JsonValue::CreateNumberValue(0));

    wdj::JsonObject settings;
    settings.Insert(L"city", wdj::JsonValue::CreateStringValue(L"Calgary"));
    settings.Insert(L"country", wdj::JsonValue::CreateStringValue(L"Canada"));
    settings.Insert(L"method", wdj::JsonValue::CreateStringValue(L"2"));
    settings.Insert(L"school", wdj::JsonValue::CreateStringValue(L"1"));
    settings.Insert(L"use24Hour", wdj::JsonValue::CreateBooleanValue(false));

    wdj::JsonObject w;
    w.Insert(L"id", wdj::JsonValue::CreateStringValue(L"com.areeb.prayer-times"));
    w.Insert(L"instanceId",
             wdj::JsonValue::CreateStringValue(L"com.areeb.prayer-times"));
    w.Insert(L"widgetId",
             wdj::JsonValue::CreateStringValue(L"com.areeb.prayer-times"));
    w.Insert(L"enabled", wdj::JsonValue::CreateBooleanValue(true));
    w.Insert(L"order", wdj::JsonValue::CreateNumberValue(0));
    w.Insert(L"position", pos);
    w.Insert(L"settings", settings);
    return w;
}

wdj::JsonObject MakeDefaultMediaWidgetJson() {
    wdj::JsonObject pos;
    pos.Insert(L"anchorPercent", wdj::JsonValue::CreateNumberValue(100));
    pos.Insert(L"offsetPx", wdj::JsonValue::CreateNumberValue(0));

    wdj::JsonObject settings;
    settings.Insert(L"darkMode", wdj::JsonValue::CreateBooleanValue(true));
    settings.Insert(L"showControls", wdj::JsonValue::CreateBooleanValue(true));
    settings.Insert(L"controlsPosition",
                    wdj::JsonValue::CreateStringValue(L"right"));
    settings.Insert(L"showVisualizer", wdj::JsonValue::CreateBooleanValue(true));
    settings.Insert(L"visualizerPosition",
                    wdj::JsonValue::CreateStringValue(L"right"));
    settings.Insert(L"visualizerBarCount",
                    wdj::JsonValue::CreateNumberValue(10));
    settings.Insert(L"showPauseOverlay",
                    wdj::JsonValue::CreateBooleanValue(true));
    settings.Insert(L"scrollingEnabled",
                    wdj::JsonValue::CreateBooleanValue(true));
    settings.Insert(L"scrollingSpeed", wdj::JsonValue::CreateNumberValue(18));

    wdj::JsonObject w;
    w.Insert(L"id", wdj::JsonValue::CreateStringValue(L"media-player"));
    w.Insert(L"instanceId", wdj::JsonValue::CreateStringValue(L"media-player"));
    w.Insert(L"widgetId", wdj::JsonValue::CreateStringValue(L"media-player"));
    w.Insert(L"enabled", wdj::JsonValue::CreateBooleanValue(false));
    w.Insert(L"order", wdj::JsonValue::CreateNumberValue(1));
    w.Insert(L"position", pos);
    w.Insert(L"settings", settings);
    return w;
}

wdj::JsonObject LoadOrCreateConfigUnlocked(AppState& app, bool& dirty) {
    dirty = false;
    std::string raw = ReadFileUtf8(app.configPath);
    wdj::JsonObject root{nullptr};
    if (raw.empty() ||
        !wdj::JsonObject::TryParse(winrt::hstring(Utf8ToWide(raw)), root)) {
        root = wdj::JsonObject();
        root.Insert(L"configVersion", wdj::JsonValue::CreateNumberValue(3));
        wdj::JsonObject layout;
        layout.Insert(L"mode", wdj::JsonValue::CreateStringValue(L"row"));
        root.Insert(L"layout", layout);

        wdj::JsonArray widgets;
        widgets.Append(MakeDefaultPrayerWidgetJson());
        widgets.Append(MakeDefaultMediaWidgetJson());
        root.Insert(L"widgets", widgets);

        wdj::JsonObject rotation;
        rotation.Insert(L"intervalSeconds",
                        wdj::JsonValue::CreateNumberValue(30));
        rotation.Insert(L"widgetIds", wdj::JsonArray());
        rotation.Insert(L"instanceIds", wdj::JsonArray());
        root.Insert(L"rotation", rotation);
        dirty = true;
        return root;
    }

    if (!root.HasKey(L"widgets") ||
        root.GetNamedValue(L"widgets").ValueType() !=
            wdj::JsonValueType::Array) {
        wdj::JsonArray widgets;
        widgets.Append(MakeDefaultPrayerWidgetJson());
        widgets.Append(MakeDefaultMediaWidgetJson());
        root.SetNamedValue(L"widgets", widgets);
        dirty = true;
    } else {
        auto arr = root.GetNamedArray(L"widgets");
        bool hasPrayer = false;
        bool hasMedia = false;
        for (uint32_t i = 0; i < arr.Size(); ++i) {
            auto item = arr.GetObjectAt(i);
            std::wstring wid = item.GetNamedString(L"widgetId", L"").c_str();
            if (wid.empty()) wid = item.GetNamedString(L"id", L"").c_str();
            if (_wcsicmp(wid.c_str(), L"com.areeb.prayer-times") == 0)
                hasPrayer = true;
            if (_wcsicmp(wid.c_str(), L"media-player") == 0) hasMedia = true;
        }
        if (!hasPrayer) {
            arr.Append(MakeDefaultPrayerWidgetJson());
            dirty = true;
        }
        if (!hasMedia) {
            arr.Append(MakeDefaultMediaWidgetJson());
            dirty = true;
        }
    }
    return root;
}

void SyncStateFromConfig(AppState& app) {
    std::lock_guard lock(app.configMutex);
    bool dirty = false;
    auto root = LoadOrCreateConfigUnlocked(app, dirty);
    if (dirty) {
        AtomicWriteUtf8(app.configPath, WideToUtf8(root.Stringify().c_str()));
    }

    auto arr = root.GetNamedArray(L"widgets");
    for (uint32_t i = 0; i < arr.Size(); ++i) {
        auto item = arr.GetObjectAt(i);
        std::wstring wid = item.GetNamedString(L"widgetId", L"").c_str();
        if (wid.empty()) wid = item.GetNamedString(L"id", L"").c_str();
        bool enabled = item.GetNamedBoolean(L"enabled", false);

        if (_wcsicmp(wid.c_str(), L"com.areeb.prayer-times") == 0) {
            app.prayerEnabled.store(enabled);
            if (item.HasKey(L"settings") &&
                item.GetNamedValue(L"settings").ValueType() ==
                    wdj::JsonValueType::Object) {
                auto s = item.GetNamedObject(L"settings");
                PrayerSettings ps;
                ps.city = s.GetNamedString(L"city", L"Calgary").c_str();
                ps.country = s.GetNamedString(L"country", L"Canada").c_str();
                ps.method = s.GetNamedString(L"method", L"2").c_str();
                ps.school = s.GetNamedString(L"school", L"1").c_str();
                ps.use24Hour = s.GetNamedBoolean(L"use24Hour", false);
                app.prayerSettings = ps;
            }
        } else if (_wcsicmp(wid.c_str(), L"media-player") == 0) {
            app.mediaEnabled.store(enabled);
        }
    }

    if (app.prayerProvider) {
        app.prayerProvider->SetEnabled(app.widgetsEnabled.load() &&
                                       app.prayerEnabled.load());
        app.prayerProvider->UpdateSettings(app.prayerSettings);
    }
}

void SetWidgetEnabledInConfig(AppState& app,
                              const std::wstring& widgetId,
                              bool enabled) {
    std::lock_guard lock(app.configMutex);
    bool dirty = false;
    auto root = LoadOrCreateConfigUnlocked(app, dirty);
    auto arr = root.GetNamedArray(L"widgets");
    for (uint32_t i = 0; i < arr.Size(); ++i) {
        auto item = arr.GetObjectAt(i);
        std::wstring wid = item.GetNamedString(L"widgetId", L"").c_str();
        if (wid.empty()) wid = item.GetNamedString(L"id", L"").c_str();
        if (_wcsicmp(wid.c_str(), widgetId.c_str()) == 0) {
            item.SetNamedValue(L"enabled",
                               wdj::JsonValue::CreateBooleanValue(enabled));
        }
    }
    AtomicWriteUtf8(app.configPath, WideToUtf8(root.Stringify().c_str()));
    if (_wcsicmp(widgetId.c_str(), L"com.areeb.prayer-times") == 0) {
        app.prayerEnabled.store(enabled);
        if (app.prayerProvider) {
            app.prayerProvider->SetEnabled(app.widgetsEnabled.load() && enabled);
        }
    } else if (_wcsicmp(widgetId.c_str(), L"media-player") == 0) {
        app.mediaEnabled.store(enabled);
    }
}

void UpdatePrayerSettingsInConfig(AppState& app, const PrayerSettings& ps) {
    std::lock_guard lock(app.configMutex);
    bool dirty = false;
    auto root = LoadOrCreateConfigUnlocked(app, dirty);
    auto arr = root.GetNamedArray(L"widgets");
    for (uint32_t i = 0; i < arr.Size(); ++i) {
        auto item = arr.GetObjectAt(i);
        std::wstring wid = item.GetNamedString(L"widgetId", L"").c_str();
        if (wid.empty()) wid = item.GetNamedString(L"id", L"").c_str();
        if (_wcsicmp(wid.c_str(), L"com.areeb.prayer-times") == 0) {
            wdj::JsonObject s;
            s.Insert(L"city", wdj::JsonValue::CreateStringValue(ps.city));
            s.Insert(L"country", wdj::JsonValue::CreateStringValue(ps.country));
            s.Insert(L"method", wdj::JsonValue::CreateStringValue(ps.method));
            s.Insert(L"school", wdj::JsonValue::CreateStringValue(ps.school));
            s.Insert(L"use24Hour",
                     wdj::JsonValue::CreateBooleanValue(ps.use24Hour));
            item.SetNamedValue(L"settings", s);
        }
    }
    AtomicWriteUtf8(app.configPath, WideToUtf8(root.Stringify().c_str()));
    app.prayerSettings = ps;
    if (app.prayerProvider) {
        app.prayerProvider->UpdateSettings(ps);
    }
}

void ApplyWidgetMoveInConfig(AppState& app,
                             const std::wstring& instanceId,
                             int anchorPercent,
                             int offsetPx) {
    anchorPercent = std::clamp(anchorPercent, 0, 100);
    offsetPx = std::clamp(offsetPx, -640, 640);
    std::lock_guard lock(app.configMutex);
    bool dirty = false;
    auto root = LoadOrCreateConfigUnlocked(app, dirty);
    auto arr = root.GetNamedArray(L"widgets");
    for (uint32_t i = 0; i < arr.Size(); ++i) {
        auto item = arr.GetObjectAt(i);
        std::wstring iid = item.GetNamedString(L"instanceId", L"").c_str();
        std::wstring id = item.GetNamedString(L"id", L"").c_str();
        if (_wcsicmp(iid.c_str(), instanceId.c_str()) == 0 ||
            _wcsicmp(id.c_str(), instanceId.c_str()) == 0) {
            wdj::JsonObject pos;
            pos.Insert(L"anchorPercent",
                       wdj::JsonValue::CreateNumberValue(anchorPercent));
            pos.Insert(L"offsetPx",
                       wdj::JsonValue::CreateNumberValue(offsetPx));
            item.SetNamedValue(L"position", pos);
            AtomicWriteUtf8(app.configPath,
                            WideToUtf8(root.Stringify().c_str()));
            return;
        }
    }
}

bool ReadWidgetsEnabled(const AppState& app) {
    std::string raw = ReadFileUtf8(app.runtimeControlPath);
    if (raw.empty()) return true;
    wdj::JsonObject obj{nullptr};
    if (!wdj::JsonObject::TryParse(winrt::hstring(Utf8ToWide(raw)), obj))
        return true;
    return obj.GetNamedBoolean(L"widgetsEnabled", true);
}

void SaveWidgetsEnabled(const AppState& app, bool enabled) {
    wdj::JsonObject obj;
    obj.Insert(L"widgetsEnabled", wdj::JsonValue::CreateBooleanValue(enabled));
    AtomicWriteUtf8(app.runtimeControlPath,
                    WideToUtf8(obj.Stringify().c_str()));
}

// ---------- Media helper control ----------

void RequestMediaControl(const AppState& app, const std::wstring& control) {
    if (fs::exists(app.mediaHelperPath)) {
        std::wstring cmd = L"\"" + app.mediaHelperPath + L"\" --" + control +
                           L" --app-dir \"" + app.dataDir + L"\"";
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, app.installDir.c_str(),
                           &si, &pi)) {
            DWORD wait = WaitForSingleObject(pi.hProcess, 1200);
            DWORD exitCode = 1;
            if (wait == WAIT_OBJECT_0 &&
                GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode == 0) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return;
            }
            if (wait != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    BYTE vk = VK_MEDIA_PLAY_PAUSE;
    if (control == L"previous") vk = VK_MEDIA_PREV_TRACK;
    if (control == L"next") vk = VK_MEDIA_NEXT_TRACK;
    keybd_event(vk, 0, 0, 0);
    keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
}

// ---------- Native Prayer Settings Dialog ----------

INT_PTR CALLBACK PrayerSettingsDlgProc(HWND dlg,
                                       UINT msg,
                                       WPARAM wParam,
                                       LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            if (!g_app) return FALSE;
            PrayerSettings ps;
            {
                std::lock_guard lock(g_app->configMutex);
                ps = g_app->prayerSettings;
            }
            SetDlgItemTextW(dlg, IDC_CITY, ps.city.c_str());
            SetDlgItemTextW(dlg, IDC_COUNTRY, ps.country.c_str());

            HWND methodCombo = GetDlgItem(dlg, IDC_METHOD);
            int methodSel = 0;
            for (int i = 0; i < static_cast<int>(ARRAYSIZE(kMethods)); ++i) {
                SendMessageW(methodCombo, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(kMethods[i].label));
                if (ps.method == kMethods[i].value) methodSel = i;
            }
            SendMessageW(methodCombo, CB_SETCURSEL, methodSel, 0);

            HWND schoolCombo = GetDlgItem(dlg, IDC_SCHOOL);
            int schoolSel = 0;
            for (int i = 0; i < static_cast<int>(ARRAYSIZE(kSchools)); ++i) {
                SendMessageW(schoolCombo, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(kSchools[i].label));
                if (ps.school == kSchools[i].value) schoolSel = i;
            }
            SendMessageW(schoolCombo, CB_SETCURSEL, schoolSel, 0);

            CheckDlgButton(dlg, IDC_USE24H,
                           ps.use24Hour ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                if (g_app) {
                    wchar_t city[128]{};
                    wchar_t country[128]{};
                    GetDlgItemTextW(dlg, IDC_CITY, city, ARRAYSIZE(city));
                    GetDlgItemTextW(dlg, IDC_COUNTRY, country,
                                    ARRAYSIZE(country));

                    int methodIdx = static_cast<int>(
                        SendDlgItemMessageW(dlg, IDC_METHOD, CB_GETCURSEL, 0, 0));
                    int schoolIdx = static_cast<int>(
                        SendDlgItemMessageW(dlg, IDC_SCHOOL, CB_GETCURSEL, 0, 0));

                    PrayerSettings ps;
                    ps.city = wcslen(city) > 0 ? city : L"Calgary";
                    ps.country = wcslen(country) > 0 ? country : L"Canada";
                    ps.method = (methodIdx >= 0 &&
                                 methodIdx < static_cast<int>(ARRAYSIZE(kMethods)))
                                    ? kMethods[methodIdx].value
                                    : L"2";
                    ps.school = (schoolIdx >= 0 &&
                                 schoolIdx < static_cast<int>(ARRAYSIZE(kSchools)))
                                    ? kSchools[schoolIdx].value
                                    : L"1";
                    ps.use24Hour =
                        IsDlgButtonChecked(dlg, IDC_USE24H) == BST_CHECKED;

                    UpdatePrayerSettingsInConfig(*g_app, ps);
                    if (g_app->prayerProvider) {
                        g_app->prayerProvider->RequestRefresh(false);
                    }
                }
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

void OpenPrayerSettingsDialog(AppState& app) {
    if (app.settingsDialogOpen) return;
    app.settingsDialogOpen = true;
    DialogBoxParamW(GetModuleHandleW(nullptr),
                    MAKEINTRESOURCEW(IDD_PRAYER_SETTINGS), app.trayHwnd,
                    PrayerSettingsDlgProc, 0);
    app.settingsDialogOpen = false;
}

// ---------- Background threads ----------

void ExplorerWatchdogThread(AppState* app) {
    while (!app->stopRequested.load()) {
        if (app->widgetsEnabled.load()) {
            for (DWORD pid : GetCurrentSessionExplorers()) {
                if (!IsHookLoadedInProcess(pid, app->hookRuntimePath)) {
                    InjectHookIntoProcess(pid, app->hookRuntimePath);
                } else {
                    SignalHookEvent(L"Local\\TaskbarWidgetsHookLoad_", pid);
                }
            }
        }
        for (int i = 0; i < 40 && !app->stopRequested.load(); ++i) {
            Sleep(100);
        }
    }
}

void MediaHelperSupervisorThread(AppState* app) {
    HANDLE helperProc = nullptr;
    auto stopHelper = [&]() {
        if (helperProc) {
            DWORD code = 0;
            if (GetExitCodeProcess(helperProc, &code) && code == STILL_ACTIVE) {
                TerminateProcess(helperProc, 0);
                WaitForSingleObject(helperProc, 2000);
                app->Log(L"Media helper stopped");
            }
            CloseHandle(helperProc);
            helperProc = nullptr;
        }
    };

    while (!app->stopRequested.load()) {
        bool wantRunning =
            app->widgetsEnabled.load() && app->mediaEnabled.load();
        if (!wantRunning) {
            stopHelper();
        } else {
            bool alive = false;
            if (helperProc) {
                DWORD code = 0;
                alive = GetExitCodeProcess(helperProc, &code) &&
                        code == STILL_ACTIVE;
                if (!alive) {
                    CloseHandle(helperProc);
                    helperProc = nullptr;
                }
            }
            if (!alive && fs::exists(app->mediaHelperPath)) {
                std::wstring cmd = L"\"" + app->mediaHelperPath +
                                   L"\" --watch --app-dir \"" + app->dataDir +
                                   L"\"";
                STARTUPINFOW si{sizeof(si)};
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                PROCESS_INFORMATION pi{};
                std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
                cmdBuf.push_back(L'\0');
                if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                                   FALSE, CREATE_NO_WINDOW, nullptr,
                                   app->installDir.c_str(), &si, &pi)) {
                    CloseHandle(pi.hThread);
                    helperProc = pi.hProcess;
                    app->Log(L"Media helper started; pid=" +
                             std::to_wstring(pi.dwProcessId));
                }
            }
        }
        for (int i = 0; i < 10 && !app->stopRequested.load(); ++i) {
            Sleep(100);
        }
    }
    stopHelper();
}

void CommandsWatcherThread(AppState* app) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    fs::file_time_type lastConfigWrite{};
    std::error_code ec;
    if (fs::exists(app->configPath, ec)) {
        lastConfigWrite = fs::last_write_time(app->configPath, ec);
    }

    while (!app->stopRequested.load()) {
        // Reload config if the user edited config.json manually
        if (fs::exists(app->configPath, ec)) {
            auto wt = fs::last_write_time(app->configPath, ec);
            if (!ec && wt != lastConfigWrite) {
                lastConfigWrite = wt;
                SyncStateFromConfig(*app);
            }
        }

        // Process hook command files in Data\Commands
        for (const auto& entry : fs::directory_iterator(app->commandsDir, ec)) {
            if (!entry.is_regular_file() ||
                entry.path().extension() != L".json") {
                continue;
            }
            std::wstring path = entry.path().wstring();
            std::string raw = ReadFileUtf8(path);
            DeleteFileW(path.c_str());
            if (raw.empty()) continue;

            wdj::JsonObject node{nullptr};
            if (!wdj::JsonObject::TryParse(winrt::hstring(Utf8ToWide(raw)),
                                           node)) {
                continue;
            }
            std::wstring action = node.GetNamedString(L"action", L"").c_str();
            std::wstring widgetId =
                node.GetNamedString(L"widgetId", L"").c_str();

            if (_wcsicmp(action.c_str(), L"mediaToggle") == 0) {
                RequestMediaControl(*app, L"toggle");
            } else if (_wcsicmp(action.c_str(), L"mediaPrevious") == 0) {
                RequestMediaControl(*app, L"previous");
            } else if (_wcsicmp(action.c_str(), L"mediaNext") == 0) {
                RequestMediaControl(*app, L"next");
            } else if (_wcsicmp(action.c_str(), L"moveWidget") == 0) {
                int pct = static_cast<int>(
                    node.GetNamedNumber(L"positionPct", 100.0));
                int off =
                    static_cast<int>(node.GetNamedNumber(L"offsetPx", 0.0));
                ApplyWidgetMoveInConfig(*app, widgetId, pct, off);
                lastConfigWrite = fs::last_write_time(app->configPath, ec);
            } else if (_wcsicmp(action.c_str(), L"disableWidget") == 0) {
                SetWidgetEnabledInConfig(*app, widgetId, false);
                lastConfigWrite = fs::last_write_time(app->configPath, ec);
            } else if (_wcsicmp(action.c_str(), L"openSettings") == 0) {
                if (app->trayHwnd) PostMessageW(app->trayHwnd, WM_OPEN_SETTINGS, 0, 0);
            } else if (_wcsicmp(action.c_str(), L"communityInvoke") == 0) {
                if (node.HasKey(L"arguments") &&
                    node.GetNamedValue(L"arguments").ValueType() ==
                        wdj::JsonValueType::Object) {
                    auto args = node.GetNamedObject(L"arguments");
                    std::wstring act =
                        args.GetNamedString(L"action", L"").c_str();
                    if (_wcsicmp(act.c_str(), L"refresh") == 0 &&
                        app->prayerProvider) {
                        app->prayerProvider->RequestRefresh(true);
                    }
                }
            } else if (_wcsicmp(action.c_str(), L"quit") == 0) {
                if (app->trayHwnd) PostMessageW(app->trayHwnd, WM_CLOSE, 0, 0);
            }
        }
        Sleep(250);
    }
    winrt::uninit_apartment();
}

// ---------- System tray window ----------

void AddTrayIcon(AppState& app) {
    app.nid = {};
    app.nid.cbSize = sizeof(app.nid);
    app.nid.hWnd = app.trayHwnd;
    app.nid.uID = 1;
    app.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    app.nid.uCallbackMessage = WM_TRAYICON;
    app.nid.hIcon = LoadIconW(GetModuleHandleW(nullptr),
                              MAKEINTRESOURCEW(IDI_APP_ICON));
    wcscpy_s(app.nid.szTip, L"Taskbar Widgets Lite");
    Shell_NotifyIconW(NIM_ADD, &app.nid);
}

void ShowTrayMenu(AppState& app) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu,
                MF_STRING | (app.widgetsEnabled.load() ? MF_CHECKED : 0),
                IDM_TOGGLE_WIDGETS, L"Show Widgets");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
                MF_STRING | (app.prayerEnabled.load() ? MF_CHECKED : 0),
                IDM_TOGGLE_PRAYER, L"Prayer Times");
    AppendMenuW(menu,
                MF_STRING | (app.mediaEnabled.load() ? MF_CHECKED : 0),
                IDM_TOGGLE_MEDIA, L"Media Player");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_PRAYER_SETTINGS,
                L"Prayer Times Settings...");
    AppendMenuW(menu, MF_STRING, IDM_REFRESH_PRAYER, L"Refresh Prayer Times");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_CONFIG,
                L"Open Config File (config.json)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : 0),
                IDM_TOGGLE_STARTUP, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(app.trayHwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0,
                   app.trayHwnd, nullptr);
    PostMessageW(app.trayHwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_app && msg == g_app->taskbarCreatedMsg && msg != 0) {
        AddTrayIcon(*g_app);
        return 0;
    }

    switch (msg) {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP ||
                lParam == WM_CONTEXTMENU) {
                if (g_app) ShowTrayMenu(*g_app);
            }
            return 0;

        case WM_OPEN_SETTINGS:
            if (g_app) OpenPrayerSettingsDialog(*g_app);
            return 0;

        case WM_COMMAND:
            if (!g_app) break;
            switch (LOWORD(wParam)) {
                case IDM_TOGGLE_WIDGETS: {
                    bool next = !g_app->widgetsEnabled.load();
                    g_app->widgetsEnabled.store(next);
                    SaveWidgetsEnabled(*g_app, next);
                    if (g_app->prayerProvider) {
                        g_app->prayerProvider->SetEnabled(
                            next && g_app->prayerEnabled.load());
                    }
                    if (!next) {
                        DetachFromAllExplorers();
                        RefreshExplorerShell();
                    } else {
                        for (DWORD pid : GetCurrentSessionExplorers()) {
                            if (!IsHookLoadedInProcess(
                                    pid, g_app->hookRuntimePath)) {
                                InjectHookIntoProcess(pid,
                                                      g_app->hookRuntimePath);
                            } else {
                                SignalHookEvent(
                                    L"Local\\TaskbarWidgetsHookLoad_", pid);
                            }
                        }
                    }
                    return 0;
                }
                case IDM_TOGGLE_PRAYER:
                    SetWidgetEnabledInConfig(*g_app, L"com.areeb.prayer-times",
                                             !g_app->prayerEnabled.load());
                    return 0;
                case IDM_TOGGLE_MEDIA:
                    SetWidgetEnabledInConfig(*g_app, L"media-player",
                                             !g_app->mediaEnabled.load());
                    return 0;
                case IDM_PRAYER_SETTINGS:
                    OpenPrayerSettingsDialog(*g_app);
                    return 0;
                case IDM_REFRESH_PRAYER:
                    if (g_app->prayerProvider) {
                        g_app->prayerProvider->RequestRefresh(true);
                    }
                    return 0;
                case IDM_OPEN_CONFIG:
                    ShellExecuteW(nullptr, L"open", g_app->configPath.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case IDM_TOGGLE_STARTUP:
                    if (IsStartupEnabled()) {
                        UninstallStartup();
                    } else {
                        InstallStartup();
                    }
                    return 0;
                case IDM_EXIT:
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
            }
            break;

        case WM_CLOSE:
            if (g_app) {
                g_app->stopRequested.store(true);
                Shell_NotifyIconW(NIM_DELETE, &g_app->nid);
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool HasArg(int argc, LPWSTR* argv, const wchar_t* flag) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], flag) == 0) return true;
    }
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (HasArg(argc, argv, L"--install-startup")) {
        InstallStartup();
        if (argv) LocalFree(argv);
        return 0;
    }
    if (HasArg(argc, argv, L"--uninstall-startup")) {
        UninstallStartup();
        if (argv) LocalFree(argv);
        return 0;
    }
    if (HasArg(argc, argv, L"--detach")) {
        SignalLoaderShutdown();
        DetachFromAllExplorers();
        WaitForProcessesToExit(L"TaskbarWidgets.exe", 6000);
        WaitForProcessesToExit(L"TaskbarWidgets.MediaHelper.exe", 4000);
        RefreshExplorerShell();
        if (argv) LocalFree(argv);
        return 0;
    }
    if (HasArg(argc, argv, L"--settings")) {
        HWND existing = FindWindowW(kTrayWindowClass, nullptr);
        if (existing) {
            PostMessageW(existing, WM_OPEN_SETTINGS, 0, 0);
            if (argv) LocalFree(argv);
            return 0;
        }
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        if (argv) LocalFree(argv);
        return 0;
    }

    AppState app;
    g_app = &app;
    app.consoleEnabled = HasArg(argc, argv, L"--console");
    if (argv) LocalFree(argv);

    if (app.consoleEnabled) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    }

    app.installDir = GetInstallDir();
    app.dataDir = app.installDir + L"\\Data";
    app.stateDir = app.dataDir + L"\\State";
    app.commandsDir = app.dataDir + L"\\Commands";
    app.logsDir = app.dataDir + L"\\Logs";
    app.runtimeDir = app.dataDir + L"\\Runtime";
    app.configPath = app.dataDir + L"\\config.json";
    app.runtimeControlPath = app.stateDir + L"\\runtime-control.json";
    app.mediaHelperPath = app.installDir + L"\\TaskbarWidgets.MediaHelper.exe";

    std::error_code ec;
    fs::create_directories(app.stateDir, ec);
    fs::create_directories(app.commandsDir, ec);
    fs::create_directories(app.logsDir, ec);
    fs::create_directories(app.runtimeDir, ec);

    app.hookRuntimePath = DeployHookDll(app);
    WriteWidgetCatalog(app);

    app.widgetsEnabled.store(ReadWidgetsEnabled(app));
    app.prayerProvider = std::make_unique<PrayerProvider>(
        app.dataDir, [&](const std::wstring& msg) { app.Log(msg); });
    SyncStateFromConfig(app);
    app.prayerProvider->Start();

    if (!app.widgetsEnabled.load()) {
        DetachFromAllExplorers();
        RefreshExplorerShell();
    }

    // Create hidden tray window
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kTrayWindowClass;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    RegisterClassExW(&wc);

    app.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    app.trayHwnd = CreateWindowExW(0, kTrayWindowClass, L"Taskbar Widgets Lite",
                                   0, 0, 0, 0, 0, nullptr, nullptr, hInstance,
                                   nullptr);
    AddTrayIcon(app);

    // Shutdown event listener thread
    HANDLE shutdownEvent =
        CreateEventW(nullptr, TRUE, FALSE, kLoaderShutdownEventName);
    if (shutdownEvent) ResetEvent(shutdownEvent);
    std::thread shutdownListener([&]() {
        if (shutdownEvent) {
            WaitForSingleObject(shutdownEvent, INFINITE);
            if (!app.stopRequested.load() && app.trayHwnd) {
                PostMessageW(app.trayHwnd, WM_CLOSE, 0, 0);
            }
        }
    });

    std::thread watchdogThread(ExplorerWatchdogThread, &app);
    std::thread mediaThread(MediaHelperSupervisorThread, &app);
    std::thread commandsThread(CommandsWatcherThread, &app);

    app.Log(L"Taskbar Widgets Lite started");

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app.stopRequested.store(true);
    if (shutdownEvent) SetEvent(shutdownEvent);
    DetachFromAllExplorers();
    RefreshExplorerShell();

    app.prayerProvider->Stop();
    if (watchdogThread.joinable()) watchdogThread.join();
    if (mediaThread.joinable()) mediaThread.join();
    if (commandsThread.joinable()) commandsThread.join();
    if (shutdownListener.joinable()) shutdownListener.join();
    if (shutdownEvent) CloseHandle(shutdownEvent);

    g_app = nullptr;
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    winrt::uninit_apartment();
    return 0;
}
