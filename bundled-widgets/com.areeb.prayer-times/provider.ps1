$ErrorActionPreference = "Stop"
[Console]::InputEncoding = [Text.Encoding]::UTF8
[Console]::OutputEncoding = [Text.Encoding]::UTF8
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Threading;
public static class PrayerInputReader
{
    private static readonly ConcurrentQueue<string> Lines = new ConcurrentQueue<string>();
    private static int started;
    private static volatile bool ended;
    public static bool Ended { get { return ended; } }
    public static void Start()
    {
        if (Interlocked.Exchange(ref started, 1) != 0) return;
        Thread t = new Thread(delegate () {
            try { string l; while ((l = Console.In.ReadLine()) != null) Lines.Enqueue(l); }
            finally { ended = true; }
        });
        t.IsBackground = true;
        t.Start();
    }
    public static bool TryRead(out string line) { return Lines.TryDequeue(out line); }
}
'@

$cacheDir = Join-Path $env:LOCALAPPDATA "TaskbarWidgets\PrayerTimes"
$instances = @{}
$settings = @{}
$days = @{}
$hijri = @{}
$lastStatus = ""
$nextPublish = [DateTime]::MinValue
$nextRetry = [DateTime]::MinValue
$shutdown = $false
$names = @("Fajr", "Sunrise", "Dhuhr", "Asr", "Maghrib", "Isha")

function Write-Json($obj) {
    [Console]::Out.WriteLine(($obj | ConvertTo-Json -Compress -Depth 8))
    [Console]::Out.Flush()
}
function Write-Log($m) { Write-Json ([ordered]@{ type = "log"; message = $m }) }

function Get-Setting($key, $default) {
    $v = $script:settings[$key]
    if ($null -eq $v -or "$v" -eq "") { return $default }
    return $v
}

function Get-CacheFile($date) {
    $key = ("{0}-{1}-{2}-{3}" -f (Get-Setting "city" "Calgary"), (Get-Setting "country" "Canada"), (Get-Setting "method" "2"), (Get-Setting "school" "1")) -replace '[^A-Za-z0-9_-]', '_'
    Join-Path $cacheDir ("{0}_{1}.json" -f $key, $date.ToString("yyyy-MM-dd"))
}

function ConvertTo-Day($timings, $date) {
    $d = @{}
    foreach ($n in $names) {
        $hhmm = ([string]$timings.$n).Substring(0, 5)
        $p = $hhmm.Split(":")
        $d[$n] = $date.Date.AddHours([int]$p[0]).AddMinutes([int]$p[1])
    }
    return $d
}

function Load-Day($date) {
    $key = $date.ToString("yyyy-MM-dd")
    if ($script:days.ContainsKey($key)) { return $true }
    $file = Get-CacheFile $date
    $json = $null
    if (Test-Path $file) {
        $json = Get-Content $file -Raw | ConvertFrom-Json
    } elseif ([DateTime]::Now -ge $script:nextRetry) {
        try {
            $url = "https://api.aladhan.com/v1/timingsByCity/{0}?city={1}&country={2}&method={3}&school={4}" -f `
                $date.ToString("dd-MM-yyyy"), [Uri]::EscapeDataString([string](Get-Setting "city" "Calgary")), `
                [Uri]::EscapeDataString([string](Get-Setting "country" "Canada")), (Get-Setting "method" "2"), (Get-Setting "school" "1")
            $resp = Invoke-RestMethod -Uri $url -TimeoutSec 15 -UserAgent "TaskbarPrayerTimes/1.0"
            if ($resp.code -ne 200) { throw "API returned code $($resp.code)" }
            $json = $resp.data
            New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
            $json | ConvertTo-Json -Depth 8 | Set-Content -Path $file -Encoding UTF8
            $script:lastStatus = ""
        } catch {
            $script:nextRetry = [DateTime]::Now.AddMinutes(2)
            $script:lastStatus = "Offline, retrying"
            Write-Log "Fetch failed: $($_.Exception.Message)"
        }
    }
    if ($null -eq $json) { return $false }
    $script:days[$key] = ConvertTo-Day $json.timings $date
    $h = $json.date.hijri
    $hm = @("Muharram","Safar","Rabi I","Rabi II","Jumada I","Jumada II","Rajab","Shaban","Ramadan","Shawwal","Dhul Qadah","Dhul Hijjah")
    $script:hijri[$key] = "{0} {1} {2}" -f $h.day, $hm[[int]$h.month.number - 1], $h.year
    return $true
}

function Format-Time([DateTime]$t) {
    if (Get-Setting "use24Hour" $false) { return $t.ToString("HH:mm") }
    return $t.ToString("h:mm tt", [Globalization.CultureInfo]::InvariantCulture)
}
function Format-Span([TimeSpan]$s) {
    $m = [Math]::Max(0, [Math]::Ceiling($s.TotalMinutes))
    if ($m -ge 60) { return "{0}h {1}m" -f [Math]::Floor($m / 60), ($m % 60) }
    return "{0}m" -f $m
}

function Publish {
    $now = [DateTime]::Now
    $today = $now.Date
    $okToday = Load-Day $today
    $okTomorrow = Load-Day $today.AddDays(1)
    $data = [ordered]@{ location = [string](Get-Setting "city" "Calgary"); accent = "#FF34D399" }
    $state = "ok"
    if (-not $okToday) {
        $data.line1 = "Prayer times"
        $data.line2 = if ($script:lastStatus) { $script:lastStatus } else { "Loading" }
        $data.status = $data.line2
        $state = "loading"
    } else {
        $okYesterday = Load-Day $today.AddDays(-1)
        $prayers = @("Fajr", "Dhuhr", "Asr", "Maghrib", "Isha")
        $events = @()
        foreach ($offset in -1, 0, 1) {
            $key = $today.AddDays($offset).ToString("yyyy-MM-dd")
            if ($script:days.ContainsKey($key)) {
                foreach ($p in $prayers) { $events += [pscustomobject]@{ Name = $p; At = $script:days[$key][$p] } }
            }
        }
        $events = @($events | Sort-Object At)
        $nextEvt = $events | Where-Object { $_.At -gt $now } | Select-Object -First 1
        $prevEvt = $events | Where-Object { $_.At -le $now } | Select-Object -Last 1
        if ($null -eq $nextEvt) { $nextEvt = [pscustomobject]@{ Name = "Fajr"; At = $today.AddDays(1).AddHours(5) } }
        $next = $nextEvt.Name; $nextAt = $nextEvt.At
        $d = $script:days[$today.ToString("yyyy-MM-dd")]
        $progress = 0
        if ($null -ne $prevEvt) {
            $total = ($nextAt - $prevEvt.At).TotalSeconds
            if ($total -gt 0) { $progress = [Math]::Round([Math]::Min(100, [Math]::Max(0, 100 * ($now - $prevEvt.At).TotalSeconds / $total)), 1) }
        }
        $data.line1 = "$next $(Format-Time $nextAt)"
        $data.line2 = "$(Format-Span ($nextAt - $now)) left"
        $data.progress = $progress
        foreach ($n in $names) {
            $data["$($n.ToLower())Time"] = Format-Time $d[$n]
            $data["$($n.ToLower())Color"] = if ($n -eq $next) { "#FF34D399" } elseif ($d[$n] -le $now) { "#FF64748B" } else { "#FFE2E8F0" }
        }
        $hj = $script:hijri[$today.ToString("yyyy-MM-dd")]
        $data.dateText = "{0}  -  {1}" -f $now.ToString("dddd, MMM d"), $hj
        $data.next = $next
        $data.status = if ($script:lastStatus) { $script:lastStatus } else { "Updated " + $now.ToString("HH:mm") }
    }
    foreach ($id in @($script:instances.Keys)) {
        Write-Json ([ordered]@{ type = "snapshot"; instanceId = $id; status = $state; data = $data })
    }
}

function Update-Instances($values) {
    $script:instances = @{}
    foreach ($i in @($values)) {
        if ($null -ne $i.instanceId) {
            $script:instances[[string]$i.instanceId] = $i.settings
            $script:settings = @{}
            if ($null -ne $i.settings) {
                foreach ($p in $i.settings.PSObject.Properties) { $script:settings[$p.Name] = $p.Value }
            }
        }
    }
    $script:days = @{}; $script:hijri = @{}
    $script:nextRetry = [DateTime]::MinValue
    $script:nextPublish = [DateTime]::MinValue
}

[PrayerInputReader]::Start()
while (-not $shutdown) {
    $line = $null
    while ([PrayerInputReader]::TryRead([ref]$line)) {
        try {
            $m = $line | ConvertFrom-Json
            switch ([string]$m.type) {
                "initialize" { Update-Instances $m.instances }
                "instancesChanged" { Update-Instances $m.instances }
                "action" {
                    if ([string]$m.action -eq "refresh") {
                        Get-ChildItem $cacheDir -Filter *.json -ErrorAction SilentlyContinue | Remove-Item -Force
                        $script:days = @{}; $script:hijri = @{}
                        $script:nextRetry = [DateTime]::MinValue
                        $script:nextPublish = [DateTime]::MinValue
                    }
                }
                "shutdown" { $shutdown = $true }
            }
        } catch { Write-Log "Input error: $($_.Exception.Message)" }
    }
    if ([PrayerInputReader]::Ended -and $null -eq $line) { break }
    if (-not $shutdown -and $instances.Count -gt 0 -and [DateTime]::Now -ge $nextPublish) {
        try { Publish } catch { Write-Log "Publish error: $($_.Exception.Message)" }
        $nextPublish = [DateTime]::Now.AddSeconds(20)
    }
    Start-Sleep -Milliseconds 200
}
