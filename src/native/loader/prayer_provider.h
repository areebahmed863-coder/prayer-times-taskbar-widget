#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct PrayerSettings {
    std::wstring city = L"Calgary";
    std::wstring country = L"Canada";
    std::wstring method = L"2";  // ISNA
    std::wstring school = L"1";  // Hanafi
    bool use24Hour = false;
};

class PrayerProvider {
public:
    using LogCallback = std::function<void(const std::wstring&)>;

    PrayerProvider(std::wstring dataDir, LogCallback log);
    ~PrayerProvider();

    void Start();
    void Stop();

    void SetEnabled(bool enabled);
    void UpdateSettings(const PrayerSettings& settings);
    void RequestRefresh(bool clearCache);

private:
    void ThreadMain();
    bool PublishOnce(bool forceClearCache);
    bool LoadDay(const SYSTEMTIME& day,
                 const PrayerSettings& settings,
                 std::string& outJson,
                 bool& attemptedNetwork,
                 std::wstring& outStatus);
    std::wstring GetCacheFilePath(const SYSTEMTIME& day,
                                  const PrayerSettings& settings) const;
    void ClearCacheFiles() const;
    bool FetchDayFromAladhan(const SYSTEMTIME& day,
                             const PrayerSettings& settings,
                             std::string& outDataJson,
                             std::wstring& outError) const;

    std::wstring dataDir_;
    std::wstring statePath_;
    std::wstring cacheDir_;
    LogCallback log_;

    std::mutex mutex_;
    PrayerSettings settings_;
    bool enabled_ = true;
    bool clearCacheRequested_ = false;
    ULONGLONG nextRetryTick_ = 0;

    HANDLE wakeEvent_ = nullptr;
    std::atomic_bool stopRequested_{false};
    std::thread worker_;
};
