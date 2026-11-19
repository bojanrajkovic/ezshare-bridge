#include "sync.h"

#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <SDFS.h>
#include <time.h>

#include <vector>

#include "config.h"
#include "ezshare.h"

namespace {

constexpr const char* kListingPath = "/.bridge/listing.tmp";

// The modification time SDFS stamps on files written while it's nonzero. Set to the
// card's timestamp during a download, so the copy's mtime records which version it is.
time_t nextFileTime = 0;

time_t fileTimeCallback() { return nextFileTime ? nextFileTime : time(nullptr); }

// shortcut: in the repeated hour when DST ends, mktime may pick the other offset, so a file
// written then downloads once more. Harmless; revisit only if it shows up in the metrics.
time_t toEpoch(const ezshare::Timestamp& t) {
    tm x{};
    x.tm_year = t.year - 1900;
    x.tm_mon = t.month - 1;
    x.tm_mday = t.day;
    x.tm_hour = t.hour;
    x.tm_min = t.minute;
    x.tm_sec = t.second;
    x.tm_isdst = -1;
    return mktime(&x);
}

struct Run {
    uint32_t deadlineMs;
    uint32_t pending = 0;
    SyncResult result = SyncResult::Ok;

    void fail(SyncResult r) {
        if (result == SyncResult::Ok) result = r;
    }
    bool outOfTime() const { return static_cast<int32_t>(millis() - deadlineMs) >= 0; }
};

// Fetches /dir?<query> to the listing file, then calls onEntry for each File or Directory
// line. Returns false if the request fails or any line is Malformed.
template <typename OnEntry>
bool listDir(const std::string& query, Run& run, OnEntry onEntry) {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(15000);
    const String url = String("http://" EZSHARE_HOST "/dir?") + query.c_str();
    if (!http.begin(client, url)) {
        run.fail(SyncResult::ListingFailed);
        return false;
    }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        run.fail(SyncResult::ListingFailed);
        return false;
    }
    fs::File out = SDFS.open(kListingPath, "w");
    if (!out) {
        http.end();
        run.fail(SyncResult::SdError);
        return false;
    }
    const int written = http.writeToStream(&out);
    out.close();
    http.end();
    if (written < 0) {
        run.fail(SyncResult::ListingFailed);
        return false;
    }

    fs::File in = SDFS.open(kListingPath, "r");
    if (!in) {
        run.fail(SyncResult::SdError);
        return false;
    }
    bool ok = true;
    while (in.available()) {
        const String line = in.readStringUntil('\n');
        const ezshare::ParsedLine parsed = ezshare::parseLine(std::string_view(line.c_str(), line.length()));
        if (std::holds_alternative<ezshare::Malformed>(parsed)) {
            Serial.printf("listing %s: unrecognized line: %s\n", query.c_str(), line.c_str());
            ok = false;
            continue;
        }
        onEntry(parsed);
    }
    in.close();
    if (!ok) run.fail(SyncResult::ListingFailed);
    return ok;
}

// Copies one file to `dir` on the microSD if the bridge's copy is missing or out of date.
void syncFile(const ezshare::File& file, const String& dir, Run& run) {
    if (file.name[0] == '.') return;
    if (!ezshare::isSafeName(file.name)) {
        Serial.printf("skipping unsafe name: %s\n", file.name.c_str());
        run.fail(SyncResult::ListingFailed);
        return;
    }
    const String path = dir + "/" + file.name.c_str();
    const time_t remoteTime = toEpoch(file.modified);

    std::optional<ezshare::LocalFile> local;
    if (fs::File existing = SDFS.open(path, "r")) {
        local = ezshare::LocalFile{existing.size(), static_cast<int64_t>(existing.getLastWrite())};
        existing.close();
    }
    if (!ezshare::needsDownload(local, file.sizeKb, remoteTime)) return;

    if (run.outOfTime()) {
        run.pending++;
        run.fail(SyncResult::TimeBudget);
        return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(30000);
    const String url = String("http://" EZSHARE_HOST "/download?") + file.query.c_str();
    if (!http.begin(client, url) || http.GET() != HTTP_CODE_OK) {
        http.end();
        run.pending++;
        run.fail(SyncResult::DownloadFailed);
        return;
    }

    const String partPath = path + ".part";
    fs::File part = SDFS.open(partPath, "w");
    if (!part) {
        http.end();
        run.pending++;
        run.fail(SyncResult::SdError);
        return;
    }
    nextFileTime = remoteTime;
    const int expected = http.getSize();
    const int written = http.writeToStream(&part);
    part.close();
    nextFileTime = 0;
    http.end();

    const bool complete = written >= 0 && (expected < 0 || written == expected) &&
                          ezshare::sizeMatchesKb(static_cast<uint64_t>(written), file.sizeKb);
    if (!complete) {
        Serial.printf("download %s: got %d of %d bytes, listed %u KB\n", path.c_str(), written, expected,
                      static_cast<unsigned>(file.sizeKb));
        SDFS.remove(partPath);
        run.pending++;
        run.fail(SyncResult::DownloadFailed);
        return;
    }

    // FAT can't rename over an existing file. The NAS keeps the previous copy meanwhile.
    SDFS.remove(path);
    if (!SDFS.rename(partPath, path)) {
        run.pending++;
        run.fail(SyncResult::SdError);
        return;
    }
    metrics.filesDownloaded++;
    metrics.bytesDownloaded += static_cast<uint64_t>(written);
}

// Lists a folder and copies every file in it to `dir`.
void syncFolder(const std::string& query, const String& dir, Run& run) {
    std::vector<ezshare::File> files;
    const bool listed = listDir(query, run, [&](const ezshare::ParsedLine& parsed) {
        if (const auto* f = std::get_if<ezshare::File>(&parsed)) files.push_back(*f);
    });
    if (!listed) return;
    SDFS.mkdir(dir);
    for (const auto& f : files) syncFile(f, dir, run);
}

void syncCard(Run& run) {
    std::vector<ezshare::File> rootFiles;
    std::optional<std::string> settingsQuery;
    std::optional<std::string> datalogQuery;
    const bool listed = listDir("dir=A:", run, [&](const ezshare::ParsedLine& parsed) {
        if (const auto* f = std::get_if<ezshare::File>(&parsed)) {
            rootFiles.push_back(*f);
        } else if (const auto* d = std::get_if<ezshare::Directory>(&parsed)) {
            if (d->name == "SETTINGS") settingsQuery = d->query;
            if (d->name == "DATALOG") datalogQuery = d->query;
        }
    });
    if (!listed) return;

    for (const auto& f : rootFiles) syncFile(f, "", run);
    if (settingsQuery) syncFolder(*settingsQuery, "/SETTINGS", run);
    if (!datalogQuery) return;

    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    const ezshare::Date today{local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};

    // Keep only the folders this run will list: the DATALOG listing grows by one line a night.
    std::vector<ezshare::Directory> folders;
    SDFS.mkdir("/DATALOG");
    if (!listDir(*datalogQuery, run, [&](const ezshare::ParsedLine& parsed) {
            const auto* d = std::get_if<ezshare::Directory>(&parsed);
            if (!d) return;
            const bool exists = SDFS.exists(String("/DATALOG/") + d->name.c_str());
            if (ezshare::shouldListDatalogFolder(d->name, today, LOOKBACK_DAYS, exists)) folders.push_back(*d);
        })) {
        return;
    }
    for (const auto& d : folders) {
        if (run.outOfTime()) {
            run.fail(SyncResult::TimeBudget);
            run.pending++;
            continue;
        }
        syncFolder(d.query, String("/DATALOG/") + d.name.c_str(), run);
    }
}

void updateSdFree() {
    fs::FSInfo64 info;
    if (SDFS.info64(info)) metrics.sdFreeBytes = info.totalBytes - info.usedBytes;
}

}  // namespace

bool connectWifi(const char* ssid, const char* password, uint32_t timeoutMs) {
    WiFi.disconnect();
    WiFi.hostname(BRIDGE_HOSTNAME);  // the core can drop it across mode changes, so set it every time
    WiFi.begin(ssid, password);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) return false;
        delay(100);
    }
    return true;
}

SyncResult runSync() {
    Run run{millis() + SYNC_TIME_BUDGET_SECONDS * 1000UL};
    SDFS.setTimeCallback(fileTimeCallback);

    if (!metrics.sdOk) {
        run.fail(SyncResult::SdError);
    } else if (!connectWifi(EZSHARE_SSID, EZSHARE_PASSWORD, JOIN_TIMEOUT_SECONDS * 1000UL)) {
        run.fail(SyncResult::JoinFailed);
    } else {
        metrics.ezshareRssi = WiFi.RSSI();
        metrics.ezshareSeen = true;
        SDFS.mkdir("/.bridge");
        syncCard(run);
    }

    // Autoreconnect keeps retrying the home network if this attempt times out.
    connectWifi(HOME_WIFI_SSID, HOME_WIFI_PASSWORD, JOIN_TIMEOUT_SECONDS * 1000UL);

    metrics.runs[static_cast<int>(run.result)]++;
    metrics.pendingFiles = run.pending;
    if (run.result == SyncResult::Ok) metrics.lastSuccessEpoch = static_cast<uint32_t>(time(nullptr));
    if (metrics.sdOk) updateSdFree();
    Serial.printf("sync: %s, %u pending\n", resultLabel(run.result), static_cast<unsigned>(run.pending));
    return run.result;
}
