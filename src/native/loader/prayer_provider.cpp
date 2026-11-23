#include "prayer_provider.h"

#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
namespace wdj = winrt::Windows::Data::Json;

namespace {

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

std::wstring UrlEncode(const std::wstring& value) {
    std::string utf8 = WideToUtf8(value);
    std::wstring out;
    out.reserve(utf8.size() * 2);
    for (unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            wchar_t buf[4]{};
            swprintf_s(buf, L"%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::wstring SanitizeFileToken(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size());
    for (wchar_t ch : value) {
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'_' || ch == L'-') {
            out.push_back(ch);
        } else {
            out.push_back(L'_');
        }
    }
    return out;
}

bool AtomicWriteUtf8(const std::wstring& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    std::wstring tmpPath = path + L".tmp";
    HANDLE file = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

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

SYSTEMTIME AddDaysToLocalDate(const SYSTEMTIME& base, int deltaDays) {
    SYSTEMTIME midnight = base;
    midnight.wHour = 12;  // Noon avoids DST transition edge cases
    midnight.wMinute = 0;
    midnight.wSecond = 0;
    midnight.wMilliseconds = 0;
    FILETIME ft{};
    SystemTimeToFileTime(&midnight, &ft);
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    constexpr long long kTicksPerDay = 24LL * 60LL * 60LL * 10000000LL;
    uli.QuadPart = static_cast<ULONGLONG>(
        static_cast<long long>(uli.QuadPart) + deltaDays * kTicksPerDay);
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME out{};
    FileTimeToSystemTime(&ft, &out);
    out.wHour = 0;
    out.wMinute = 0;
    out.wSecond = 0;
    out.wMilliseconds = 0;
    return out;
}

long long LocalSystemTimeToEpochSeconds(const SYSTEMTIME& st) {
    std::tm tmVal{};
    tmVal.tm_year = st.wYear - 1900;
    tmVal.tm_mon = st.wMonth - 1;
    tmVal.tm_mday = st.wDay;
    tmVal.tm_hour = st.wHour;
    tmVal.tm_min = st.wMinute;
    tmVal.tm_sec = st.wSecond;
    tmVal.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&tmVal));
}

bool ParseHHMM(const std::wstring& text, int& hour, int& minute) {
    if (text.size() < 5) return false;
    if (text[2] != L':') return false;
    if (!iswdigit(text[0]) || !iswdigit(text[1]) || !iswdigit(text[3]) ||
        !iswdigit(text[4])) {
        return false;
    }
    hour = (text[0] - L'0') * 10 + (text[1] - L'0');
    minute = (text[3] - L'0') * 10 + (text[4] - L'0');
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

std::wstring FormatClock(int hour, int minute, bool use24Hour) {
    wchar_t buf[32]{};
    if (use24Hour) {
        swprintf_s(buf, L"%02d:%02d", hour, minute);
        return buf;
    }
    const wchar_t* ampm = hour >= 12 ? L"PM" : L"AM";
    int h12 = hour % 12;
    if (h12 == 0) h12 = 12;
    swprintf_s(buf, L"%d:%02d %s", h12, minute, ampm);
    return buf;
}

std::wstring FormatSpan(long long seconds) {
    long long minutes =
        std::max<long long>(0, (seconds + 59) / 60);  // Ceiling minutes
    wchar_t buf[32]{};
    if (minutes >= 60) {
        swprintf_s(buf, L"%lldh %lldm", minutes / 60, minutes % 60);
    } else {
        swprintf_s(buf, L"%lldm", minutes);
    }
    return buf;
}

struct DayTimings {
    struct Entry {
        std::wstring name;
        int hour = 0;
        int minute = 0;
        long long epoch = 0;
    };
    std::array<Entry, 6> entries;  // Fajr, Sunrise, Dhuhr, Asr, Maghrib, Isha
    std::wstring hijriText;
    bool valid = false;
};

DayTimings ParseDayJson(const std::string& jsonUtf8, const SYSTEMTIME& day) {
    DayTimings result{};
    static const std::array<std::wstring, 6> kNames = {
        L"Fajr", L"Sunrise", L"Dhuhr", L"Asr", L"Maghrib", L"Isha"};
    static const std::array<const wchar_t*, 12> kHijriMonths = {
        L"Muharram", L"Safar",    L"Rabi I",      L"Rabi II",
        L"Jumada I", L"Jumada II", L"Rajab",      L"Shaban",
        L"Ramadan",  L"Shawwal",  L"Dhul Qadah",  L"Dhul Hijjah"};

    wdj::JsonObject root{nullptr};
    if (!wdj::JsonObject::TryParse(winrt::hstring(Utf8ToWide(jsonUtf8)),
                                   root)) {
        return result;
    }
    if (!root.HasKey(L"timings")) return result;
    auto timings = root.GetNamedObject(L"timings");

    for (size_t i = 0; i < kNames.size(); ++i) {
        if (!timings.HasKey(kNames[i])) return result;
        std::wstring raw = timings.GetNamedString(kNames[i]).c_str();
        int h = 0, m = 0;
        if (!ParseHHMM(raw, h, m)) return result;
        SYSTEMTIME st = day;
        st.wHour = static_cast<WORD>(h);
        st.wMinute = static_cast<WORD>(m);
        st.wSecond = 0;
        st.wMilliseconds = 0;
        result.entries[i] = {kNames[i], h, m, LocalSystemTimeToEpochSeconds(st)};
    }

    try {
        if (root.HasKey(L"date")) {
            auto dateObj = root.GetNamedObject(L"date");
            if (dateObj.HasKey(L"hijri")) {
                auto hijri = dateObj.GetNamedObject(L"hijri");
                std::wstring hDay = hijri.GetNamedString(L"day", L"").c_str();
                std::wstring hYear = hijri.GetNamedString(L"year", L"").c_str();
                int monthNum = 1;
                if (hijri.HasKey(L"month")) {
                    auto monthObj = hijri.GetNamedObject(L"month");
                    monthNum =
                        static_cast<int>(monthObj.GetNamedNumber(L"number", 1));
                }
                if (monthNum >= 1 && monthNum <= 12 && !hDay.empty() &&
                    !hYear.empty()) {
                    result.hijriText =
                        hDay + L" " + kHijriMonths[monthNum - 1] + L" " + hYear;
                }
            }
        }
    } catch (...) {
    }

    result.valid = true;
    return result;
}

std::wstring FormatGregorianHeader(const SYSTEMTIME& now) {
    static const wchar_t* kDays[] = {L"Sunday",   L"Monday",   L"Tuesday",
                                     L"Wednesday", L"Thursday", L"Friday",
                                     L"Saturday"};
    static const wchar_t* kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr",
                                       L"May", L"Jun", L"Jul", L"Aug",
                                       L"Sep", L"Oct", L"Nov", L"Dec"};
    wchar_t buf[64]{};
    swprintf_s(buf, L"%s, %s %u", kDays[now.wDayOfWeek % 7],
               kMonths[(now.wMonth - 1) % 12], now.wDay);
    return buf;
}

}  // namespace

PrayerProvider::PrayerProvider(std::wstring dataDir, LogCallback log)
    : dataDir_(std::move(dataDir)),
      statePath_(dataDir_ + L"\\State\\com.areeb.prayer-times.json"),
      cacheDir_(dataDir_ + L"\\Cache\\PrayerTimes"),
      log_(std::move(log)) {
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

PrayerProvider::~PrayerProvider() {
    Stop();
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
}

void PrayerProvider::Start() {
    stopRequested_.store(false);
    worker_ = std::thread(&PrayerProvider::ThreadMain, this);
}

void PrayerProvider::Stop() {
    stopRequested_.store(true);
    if (wakeEvent_) {
        SetEvent(wakeEvent_);
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void PrayerProvider::SetEnabled(bool enabled) {
    {
        std::lock_guard lock(mutex_);
        if (enabled_ == enabled) return;
        enabled_ = enabled;
        nextRetryTick_ = 0;
    }
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void PrayerProvider::UpdateSettings(const PrayerSettings& settings) {
    {
        std::lock_guard lock(mutex_);
        settings_ = settings;
        nextRetryTick_ = 0;
    }
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void PrayerProvider::RequestRefresh(bool clearCache) {
    {
        std::lock_guard lock(mutex_);
        if (clearCache) clearCacheRequested_ = true;
        nextRetryTick_ = 0;
    }
    if (wakeEvent_) SetEvent(wakeEvent_);
}

std::wstring PrayerProvider::GetCacheFilePath(
    const SYSTEMTIME& day, const PrayerSettings& settings) const {
    wchar_t dateBuf[16]{};
    swprintf_s(dateBuf, L"%04u-%02u-%02u", day.wYear, day.wMonth, day.wDay);
    std::wstring key =
        SanitizeFileToken(settings.city + L"-" + settings.country + L"-" +
                          settings.method + L"-" + settings.school);
    return cacheDir_ + L"\\" + key + L"_" + dateBuf + L".json";
}

void PrayerProvider::ClearCacheFiles() const {
    std::error_code ec;
    if (!fs::exists(cacheDir_, ec)) return;
    for (const auto& entry : fs::directory_iterator(cacheDir_, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == L".json") {
            fs::remove(entry.path(), ec);
        }
    }
}

bool PrayerProvider::FetchDayFromAladhan(const SYSTEMTIME& day,
                                         const PrayerSettings& settings,
                                         std::string& outDataJson,
                                         std::wstring& outError) const {
    wchar_t datePath[32]{};
    swprintf_s(datePath, L"%02u-%02u-%04u", day.wDay, day.wMonth, day.wYear);

    std::wstring path = L"/v1/timingsByCity/";
    path += datePath;
    path += L"?city=" + UrlEncode(settings.city);
    path += L"&country=" + UrlEncode(settings.country);
    path += L"&method=" + UrlEncode(settings.method);
    path += L"&school=" + UrlEncode(settings.school);

    HINTERNET session = WinHttpOpen(L"TaskbarPrayerTimes/2.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        outError = L"WinHttpOpen failed";
        return false;
    }

    WinHttpSetTimeouts(session, 8000, 8000, 10000, 12000);

    HINTERNET connect =
        WinHttpConnect(session, L"api.aladhan.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        outError = L"WinHttpConnect failed";
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        outError = L"WinHttpOpenRequest failed";
        return false;
    }

    std::string body;
    bool ok = false;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(
            request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
            WINHTTP_NO_HEADER_INDEX);

        if (statusCode == 200) {
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) &&
                   available > 0) {
                size_t oldSize = body.size();
                body.resize(oldSize + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, body.data() + oldSize, available,
                                     &read)) {
                    body.clear();
                    break;
                }
                body.resize(oldSize + read);
                if (body.size() > 2 * 1024 * 1024) {
                    body.clear();
                    break;
                }
            }
            ok = !body.empty();
        } else {
            outError = L"HTTP " + std::to_wstring(statusCode);
        }
    } else {
        outError = L"Offline, retrying";
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!ok) return false;

    wdj::JsonObject parsed{nullptr};
    if (!wdj::JsonObject::TryParse(winrt::hstring(Utf8ToWide(body)), parsed)) {
        outError = L"Invalid API JSON";
        return false;
    }
    if (static_cast<int>(parsed.GetNamedNumber(L"code", 0)) != 200 ||
        !parsed.HasKey(L"data")) {
        outError = L"API returned error";
        return false;
    }

    auto dataObj = parsed.GetNamedObject(L"data");
    outDataJson = WideToUtf8(dataObj.Stringify().c_str());
    return !outDataJson.empty();
}

bool PrayerProvider::LoadDay(const SYSTEMTIME& day,
                             const PrayerSettings& settings,
                             std::string& outJson,
                             bool& attemptedNetwork,
                             std::wstring& outStatus) {
    std::wstring cacheFile = GetCacheFilePath(day, settings);
    outJson = ReadFileUtf8(cacheFile);
    if (!outJson.empty()) {
        return true;
    }

    ULONGLONG nowTick = GetTickCount64();
    if (nowTick < nextRetryTick_) {
        outStatus = L"Offline, retrying";
        return false;
    }

    attemptedNetwork = true;
    std::wstring err;
    if (FetchDayFromAladhan(day, settings, outJson, err)) {
        AtomicWriteUtf8(cacheFile, outJson);
        outStatus.clear();
        return true;
    }

    nextRetryTick_ = GetTickCount64() + 120000ULL;  // Retry after 2 min
    outStatus = err.empty() ? L"Offline, retrying" : err;
    if (log_) {
        log_(L"Prayer fetch failed: " + outStatus);
    }
    return false;
}

bool PrayerProvider::PublishOnce(bool forceClearCache) {
    PrayerSettings settings;
    bool enabled = false;
    {
        std::lock_guard lock(mutex_);
        settings = settings_;
        enabled = enabled_;
    }

    if (!enabled) return false;

    if (forceClearCache) {
        ClearCacheFiles();
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    SYSTEMTIME today = now;
    today.wHour = 0;
    today.wMinute = 0;
    today.wSecond = 0;
    today.wMilliseconds = 0;
    SYSTEMTIME yesterday = AddDaysToLocalDate(today, -1);
    SYSTEMTIME tomorrow = AddDaysToLocalDate(today, 1);

    bool attemptedNetwork = false;
    std::wstring lastStatus;
    std::string todayJson, tomorrowJson, yesterdayJson;

    bool okToday =
        LoadDay(today, settings, todayJson, attemptedNetwork, lastStatus);
    DayTimings todayTimings =
        okToday ? ParseDayJson(todayJson, today) : DayTimings{};

    wdj::JsonObject data;
    data.Insert(L"location", wdj::JsonValue::CreateStringValue(settings.city));
    data.Insert(L"accent", wdj::JsonValue::CreateStringValue(L"#FF34D399"));

    std::wstring state = L"ok";
    if (!todayTimings.valid) {
        state = L"loading";
        data.Insert(L"line1",
                    wdj::JsonValue::CreateStringValue(L"Prayer times"));
        std::wstring msg = lastStatus.empty() ? L"Loading" : lastStatus;
        data.Insert(L"line2", wdj::JsonValue::CreateStringValue(msg));
        data.Insert(L"progress", wdj::JsonValue::CreateNumberValue(0));
        data.Insert(L"status", wdj::JsonValue::CreateStringValue(msg));
    } else {
        std::wstring ignoreStatus;
        bool okTomorrow = LoadDay(tomorrow, settings, tomorrowJson,
                                  attemptedNetwork, ignoreStatus);
        bool okYesterday = LoadDay(yesterday, settings, yesterdayJson,
                                   attemptedNetwork, ignoreStatus);

        DayTimings tomorrowTimings =
            okTomorrow ? ParseDayJson(tomorrowJson, tomorrow) : DayTimings{};
        DayTimings yesterdayTimings =
            okYesterday ? ParseDayJson(yesterdayJson, yesterday) : DayTimings{};

        struct PrayerEvent {
            std::wstring name;
            int hour;
            int minute;
            long long epoch;
        };
        std::vector<PrayerEvent> events;
        auto pushFive = [&](const DayTimings& dt) {
            if (!dt.valid) return;
            // Skip index 1 (Sunrise) for next-prayer countdown
            for (size_t idx : {0u, 2u, 3u, 4u, 5u}) {
                events.push_back({dt.entries[idx].name, dt.entries[idx].hour,
                                  dt.entries[idx].minute,
                                  dt.entries[idx].epoch});
            }
        };
        pushFive(yesterdayTimings);
        pushFive(todayTimings);
        pushFive(tomorrowTimings);
        std::sort(events.begin(), events.end(),
                  [](const PrayerEvent& a, const PrayerEvent& b) {
                      return a.epoch < b.epoch;
                  });

        long long nowEpoch = LocalSystemTimeToEpochSeconds(now);
        const PrayerEvent* nextEvt = nullptr;
        const PrayerEvent* prevEvt = nullptr;
        for (const auto& ev : events) {
            if (ev.epoch <= nowEpoch) {
                prevEvt = &ev;
            } else if (!nextEvt) {
                nextEvt = &ev;
                break;
            }
        }

        std::wstring nextName = L"Fajr";
        std::wstring nextClock = FormatClock(5, 0, settings.use24Hour);
        long long nextEpoch = nowEpoch + 3600;
        if (nextEvt) {
            nextName = nextEvt->name;
            nextClock =
                FormatClock(nextEvt->hour, nextEvt->minute, settings.use24Hour);
            nextEpoch = nextEvt->epoch;
        }

        double progress = 0.0;
        if (prevEvt && nextEpoch > prevEvt->epoch) {
            double frac = static_cast<double>(nowEpoch - prevEvt->epoch) /
                          static_cast<double>(nextEpoch - prevEvt->epoch);
            progress =
                std::round(std::clamp(frac * 100.0, 0.0, 100.0) * 10.0) / 10.0;
        }

        data.Insert(L"line1",
                    wdj::JsonValue::CreateStringValue(nextName + L" " + nextClock));
        data.Insert(L"line2", wdj::JsonValue::CreateStringValue(
                                  FormatSpan(nextEpoch - nowEpoch) + L" left"));
        data.Insert(L"progress", wdj::JsonValue::CreateNumberValue(progress));
        data.Insert(L"next", wdj::JsonValue::CreateStringValue(nextName));

        static const std::array<std::wstring, 6> kLower = {
            L"fajr", L"sunrise", L"dhuhr", L"asr", L"maghrib", L"isha"};
        for (size_t i = 0; i < 6; ++i) {
            const auto& e = todayTimings.entries[i];
            data.Insert(
                kLower[i] + L"Time",
                wdj::JsonValue::CreateStringValue(
                    FormatClock(e.hour, e.minute, settings.use24Hour)));
            std::wstring color = L"#FFE2E8F0";
            if (e.name == nextName) {
                color = L"#FF34D399";
            } else if (e.epoch <= nowEpoch) {
                color = L"#FF64748B";
            }
            data.Insert(kLower[i] + L"Color",
                        wdj::JsonValue::CreateStringValue(color));
        }

        std::wstring dateText = FormatGregorianHeader(now);
        if (!todayTimings.hijriText.empty()) {
            dateText += L"  -  " + todayTimings.hijriText;
        }
        data.Insert(L"dateText", wdj::JsonValue::CreateStringValue(dateText));

        wchar_t upd[32]{};
        swprintf_s(upd, L"Updated %02u:%02u", now.wHour, now.wMinute);
        data.Insert(L"status", wdj::JsonValue::CreateStringValue(upd));
    }

    wdj::JsonObject envelope;
    envelope.Insert(L"status", wdj::JsonValue::CreateStringValue(state));
    envelope.Insert(L"data", data);

    AtomicWriteUtf8(statePath_, WideToUtf8(envelope.Stringify().c_str()));
    return true;
}

void PrayerProvider::ThreadMain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (!stopRequested_.load()) {
        bool clearCache = false;
        {
            std::lock_guard lock(mutex_);
            clearCache = clearCacheRequested_;
            clearCacheRequested_ = false;
        }
        try {
            PublishOnce(clearCache);
        } catch (...) {
            if (log_) log_(L"PrayerProvider publish exception");
        }
        WaitForSingleObject(wakeEvent_, 20000);
    }
    winrt::uninit_apartment();
}
