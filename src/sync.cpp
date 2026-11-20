#include "sync.h"

#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <SDFS.h>
#include <time.h>

#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "ezshare.h"
#include "log.h"

namespace {

using ezshare::ProbeStatus;
using ezshare::Signature;

constexpr uint16_t kProbeTimeoutMs = 5000;
constexpr uint16_t kDownloadTimeoutMs = 30000;
constexpr int kDownloadAttempts = 4;  // the first try plus 3 resumes
constexpr const char* kSessionTypes[] = {"BRP", "PLD", "SAD", "EVE", "CSL"};

struct Run {
    uint32_t deadlineMs;
    uint32_t pending = 0;
    uint32_t probes = 0;
    uint32_t folders = 0;
    uint32_t failures = 0;
    SyncResult result = SyncResult::Ok;

    void fail(SyncResult r) {
        failures++;
        if (result == SyncResult::Ok) result = r;
    }
    bool outOfTime() const { return static_cast<int32_t>(millis() - deadlineMs) >= 0; }
    // Stops before more work: counts what's left and records the time budget outcome.
    bool stopForTime() {
        if (!outOfTime()) return false;
        pending++;
        fail(SyncResult::TimeBudget);
        return true;
    }
};

// One folder on the card: its fdir argument, and where its copies go on the microSD.
struct Folder {
    String fdir;   // e.g. "..%5CDATALOG%5C20260221"; the root is "..", with no trailing separator
    String local;  // e.g. "/DATALOG/20260221"; the root is ""
};

struct ProbeReply {
    ProbeStatus status;
    Signature signature;  // set on a hit
};

String downloadUrl(const Folder& folder, const std::string& fname) {
    // fname goes in unencoded: the card hangs on %3F, and names and patterns hold no other
    // characters that need encoding.
    return String("http://" EZSHARE_HOST "/download?fname=") + fname.c_str() + "&fdir=" + folder.fdir;
}

// Asks the card for the bytes at signatureRange(pattern) of the first file matching
// `pattern`. 206 is a hit, 404 a miss, anything else a failure.
ProbeReply probe(const Folder& folder, const std::string& pattern, Run& run) {
    run.probes++;
    metrics.probes++;
    const ezshare::ByteRange range = ezshare::signatureRange(pattern);
    static const char* kHeaders[] = {"Content-Range"};

    WiFiClient client;
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(kProbeTimeoutMs);
    int code = -1;
    if (http.begin(client, downloadUrl(folder, pattern))) {
        http.addHeader("Range", "bytes=" + String(range.first) + "-" + String(range.last));
        http.collectHeaders(kHeaders, 1);
        code = http.GET();
    }
    ProbeReply reply{ProbeStatus::Failed, {}};
    if (code == HTTP_CODE_NOT_FOUND) {
        reply.status = ProbeStatus::Miss;
    } else if (code == HTTP_CODE_PARTIAL_CONTENT) {
        const String cr = http.header("Content-Range");
        const String body = http.getString();
        if (const auto total = ezshare::contentRangeTotal(std::string_view(cr.c_str(), cr.length()))) {
            reply.status = ProbeStatus::Hit;
            reply.signature.size = *total;
            if (ezshare::keepsSignatureBytes(pattern)) reply.signature.bytes.assign(body.c_str(), body.length());
        }
    }
    http.end();

    if (reply.status == ProbeStatus::Failed) {
        logEvent("error", "probe_failed", {text("pattern", pattern.c_str()), text("folder", folder.local.isEmpty() ? String("/") : folder.local), num("status", code)});
        run.fail(SyncResult::ProbeFailed);
    }
    return reply;
}

ProbeStatus probeExists(const Folder& folder, const std::string& pattern, Run& run) {
    return probe(folder, pattern, run).status;
}

// The signature of the bridge's copy, computed the same way the card's is.
std::optional<Signature> localSignature(const Folder& folder, const std::string& name) {
    fs::File f = SDFS.open(folder.local + "/" + name.c_str(), "r");
    if (!f) return std::nullopt;
    Signature s{f.size(), ""};
    if (ezshare::keepsSignatureBytes(name)) {
        const ezshare::ByteRange range = ezshare::signatureRange(name);
        char buf[8];
        if (f.seek(range.first)) s.bytes.assign(buf, f.read(reinterpret_cast<uint8_t*>(buf), range.last - range.first + 1));
    }
    f.close();
    return s;
}

// Streams `name` into <name>.part, resuming with a Range request when a transfer stalls or
// comes up short, and renames it over the copy once it has all `total` bytes.
bool download(const Folder& folder, const std::string& name, uint64_t total, Run& run) {
    if (run.stopForTime()) return false;
    const String path = folder.local + "/" + name.c_str();
    const String partPath = path + ".part";

    int code = -1;
    for (int attempt = 0; attempt < kDownloadAttempts; ++attempt) {
        fs::File part = SDFS.open(partPath, attempt == 0 ? "w" : "a");
        if (!part) {
            logEvent("error", "sd_error", {text("op", "write"), text("path", partPath)});
            run.pending++;
            run.fail(SyncResult::SdError);
            return false;
        }
        const size_t have = part.size();
        const int expected = have > 0 ? HTTP_CODE_PARTIAL_CONTENT : HTTP_CODE_OK;

        WiFiClient client;
        HTTPClient http;
        http.setReuse(false);
        http.setTimeout(kDownloadTimeoutMs);
        code = -1;
        if (http.begin(client, downloadUrl(folder, name))) {
            if (have > 0) http.addHeader("Range", "bytes=" + String(have) + "-");
            code = http.GET();
        }
        if (code == expected) http.writeToStream(&part);
        const size_t got = part.size();
        part.close();
        http.end();

        if (code > 0 && code != expected) break;
        if (got == total) {
            // FAT can't rename over an existing file. The puller keeps the previous copy meanwhile.
            SDFS.remove(path);
            if (!SDFS.rename(partPath, path)) {
                logEvent("error", "sd_error", {text("op", "rename"), text("path", path)});
                run.pending++;
                run.fail(SyncResult::SdError);
                return false;
            }
            metrics.filesDownloaded++;
            metrics.bytesDownloaded += total;
            return true;
        }
        logEvent("warn", "download_stalled",
                 {text("path", path), num("bytes", got), num("total", static_cast<int64_t>(total))});
        if (got > total) break;
    }
    logEvent("error", "download_failed", {text("path", path), num("status", code)});
    SDFS.remove(partPath);
    run.pending++;
    run.fail(SyncResult::DownloadFailed);
    return false;
}

// Probes `name` and its .crc companion, if it has one, and copies whichever changed. The
// .crc is copied after the file it checksums, so a failed download is retried next run.
void syncFile(const Folder& folder, const std::string& name, Run& run) {
    const std::string crc = ezshare::crcCompanion(name);
    std::optional<Signature> crcRemote;
    bool crcChanged = false;
    if (!crc.empty()) {
        const ProbeReply r = probe(folder, crc, run);
        if (r.status == ProbeStatus::Failed) return;
        if (r.status == ProbeStatus::Hit) {
            crcRemote = r.signature;
            crcChanged = ezshare::needsDownload(localSignature(folder, crc), r.signature, false);
        }
    }

    const ProbeReply r = probe(folder, name, run);
    if (r.status == ProbeStatus::Failed) return;
    bool ok = true;
    if (r.status == ProbeStatus::Hit &&
        ezshare::needsDownload(localSignature(folder, name), r.signature, crcChanged)) {
        ok = download(folder, name, r.signature.size, run);
    }
    if (ok && crcChanged) download(folder, crc, crcRemote->size, run);
}

void syncRoot(Run& run) {
    const Folder root{"..", ""};
    syncFile(root, "Identification.tgt", run);
    syncFile(root, "STR.edf", run);
}

void syncSettings(Run& run) {
    const Folder settings{"..%5CSETTINGS", "/SETTINGS"};
    std::vector<std::string> stems;
    const auto exists = [&](const std::string& p) { return probeExists(settings, p, run); };
    if (!ezshare::expand("", std::vector<std::string_view>(3, ezshare::kLetters), exists, stems)) return;
    SDFS.mkdir(settings.local);
    for (const auto& stem : stems) {
        if (run.stopForTime()) return;
        syncFile(settings, stem + ".tgt", run);
        syncFile(settings, stem + ".log", run);
    }
}

// Copies one night's folder. A folder the bridge doesn't have yet is copied into a hidden
// .<date> folder and renamed when the run finishes it with no failures, so an unfinished
// copy is never mistaken for a finished one, and the puller sees a night all at once.
void syncNight(ezshare::Date night, Run& run) {
    const std::string name = ezshare::folderName(night);
    const String finalPath = String("/DATALOG/") + name.c_str();
    const bool staged = !SDFS.exists(finalPath);
    const Folder folder{String("..%5CDATALOG%5C") + name.c_str(),
                        staged ? String("/DATALOG/.") + name.c_str() : finalPath};

    if (probeExists(folder, "*", run) != ProbeStatus::Hit) return;
    run.folders++;
    const uint32_t failuresBefore = run.failures;

    std::vector<std::string> stamps;
    const auto exists = [&](const std::string& p) { return probeExists(folder, p, run); };
    if (!ezshare::expand(name + "_", ezshare::timeAlphabets(false), exists, stamps)) return;
    const std::string nextDay = ezshare::folderName(ezshare::addDays(night, 1));
    if (!ezshare::expand(nextDay + "_", ezshare::timeAlphabets(true), exists, stamps)) return;

    SDFS.mkdir(folder.local);
    for (const auto& stamp : stamps) {
        for (const char* type : kSessionTypes) {
            if (run.stopForTime()) return;
            syncFile(folder, stamp + "_" + type + ".edf", run);
        }
    }
    if (staged && run.failures == failuresBefore && !SDFS.rename(folder.local, finalPath)) {
        logEvent("error", "sd_error", {text("op", "rename"), text("path", finalPath)});
        run.fail(SyncResult::SdError);
    }
}

// The first day the card has data for, from the start date in the bridge's copy of STR.edf.
std::optional<ezshare::Date> cardStartDate() {
    fs::File f = SDFS.open("/STR.edf", "r");
    if (!f) return std::nullopt;
    char buf[8];
    const size_t n = f.seek(168) ? f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf)) : 0;
    f.close();
    return ezshare::parseEdfStartDate(std::string_view(buf, n));
}

// Night folders newest first: the lookback window every run, then any older night the
// bridge hasn't finished, back to the card's start date.
void syncDatalog(Run& run) {
    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    const ezshare::Date today{local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
    ezshare::Date oldest = ezshare::addDays(today, -LOOKBACK_DAYS);
    if (const auto start = cardStartDate(); start && ezshare::daysBetween(*start, oldest) > 0) oldest = *start;

    SDFS.mkdir("/DATALOG");
    for (ezshare::Date d = today; ezshare::daysBetween(oldest, d) >= 0; d = ezshare::addDays(d, -1)) {
        const std::string name = ezshare::folderName(d);
        const bool exists = SDFS.exists(String("/DATALOG/") + name.c_str());
        if (!ezshare::shouldListDatalogFolder(name, today, LOOKBACK_DAYS, exists)) continue;
        if (run.stopForTime()) return;
        syncNight(d, run);
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
    const uint32_t startMs = millis();
    const uint32_t filesBefore = metrics.filesDownloaded;
    const uint64_t bytesBefore = metrics.bytesDownloaded;
    Run run{startMs + SYNC_TIME_BUDGET_SECONDS * 1000UL};

    if (!metrics.sdOk) {
        run.fail(SyncResult::SdError);
    } else if (!connectWifi(EZSHARE_SSID, EZSHARE_PASSWORD, JOIN_TIMEOUT_SECONDS * 1000UL)) {
        logEvent("error", "join_failed", {text("ssid", EZSHARE_SSID), num("wifi_status", WiFi.status())});
        run.fail(SyncResult::JoinFailed);
    } else {
        metrics.ezshareRssi = WiFi.RSSI();
        metrics.ezshareSeen = true;
        syncRoot(run);
        syncSettings(run);
        syncDatalog(run);
    }

    // Autoreconnect keeps retrying the home network if this attempt times out.
    connectWifi(HOME_WIFI_SSID, HOME_WIFI_PASSWORD, JOIN_TIMEOUT_SECONDS * 1000UL);

    metrics.runs[static_cast<int>(run.result)]++;
    metrics.pendingFiles = run.pending;
    metrics.lastSyncSeconds = (millis() - startMs) / 1000;
    if (run.result == SyncResult::Ok) metrics.lastSuccessEpoch = static_cast<uint32_t>(time(nullptr));
    if (metrics.sdOk) updateSdFree();
    const char* level = run.result == SyncResult::Ok           ? "info"
                        : run.result == SyncResult::TimeBudget ? "warn"
                                                               : "error";
    logEvent(level, "sync",
             {text("result", resultLabel(run.result)), num("files", metrics.filesDownloaded - filesBefore),
              num("bytes", static_cast<int64_t>(metrics.bytesDownloaded - bytesBefore)), num("pending", run.pending),
              num("probes", run.probes), num("folders", run.folders), num("seconds", metrics.lastSyncSeconds),
              num("ezshare_rssi", metrics.ezshareRssi)});
    return run.result;
}
