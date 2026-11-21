#include "sync.h"

#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <SDFS.h>
#include <time.h>

#include <algorithm>
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

// The SdFat object behind SDFS, for what SDFS doesn't expose: 64-bit sizes and the card's error
// code. It's protected, twice; naming each accessor through a derived class makes it reachable
// with a member pointer, which is legal on the base object.
struct FsAccess : fs::FS {
    using fs::FS::getImpl;
};
struct SdfsAccess : sdfs::SDFSImpl {
    using sdfs::SDFSImpl::getFs;
};

SdFat* sdFat() {
    auto* impl = static_cast<sdfs::SDFSImpl*>((SDFS.*(&FsAccess::getImpl))().get());
    return (impl->*(&SdfsAccess::getFs))();
}

String downloadUrl(const Folder& folder, const std::string& fname) {
    // fname goes in unencoded: the card hangs on %3F, and names and patterns hold no other
    // characters that need encoding.
    return String("http://" EZSHARE_HOST "/download?fname=") + fname.c_str() + "&fdir=" + folder.fdir;
}

// Asks the card for the bytes at signatureRange(pattern) of the first file matching
// `pattern`. 206 is a hit, 404 a miss, anything else a failure.
ProbeReply probe(const Folder& folder, const std::string& pattern, Run& run) {
    const ezshare::ByteRange range = ezshare::signatureRange(pattern);
    static const char* kHeaders[] = {"Content-Range"};
    // The card closes the connection after every reply, so each probe opens its own.
    WiFiClient client;
    HTTPClient http;
    int code = -1;
    // Retry a connection that failed or timed out, which a weak signal causes. A 503 isn't
    // retried: it means every download slot is stuck, and only a power cycle frees them.
    for (uint32_t backoffMs = 500; backoffMs <= 8000; backoffMs *= 2) {
        run.probes++;
        metrics.probes++;
        http.setReuse(false);
        http.setTimeout(kProbeTimeoutMs);
        code = -1;
        if (http.begin(client, downloadUrl(folder, pattern))) {
            if (ezshare::sendsRange(pattern)) {
                http.addHeader("Range", "bytes=" + String(range.first) + "-" + String(range.last));
            }
            http.collectHeaders(kHeaders, 1);
            const uint32_t sentMs = millis();
            code = http.GET();
#ifdef EZB_DEBUG_PROBES
            logEvent("debug", "probe", {text("folder", folder.local.isEmpty() ? String("/") : folder.local),
                                        text("pattern", pattern.c_str()), num("status", code),
                                        num("ms", millis() - sentMs), num("size", http.getSize()),
                                        text("content_range", http.header("Content-Range")),
                                        text("body", code == HTTP_CODE_SERVICE_UNAVAILABLE ? http.getString() : String())});
#else
            (void)sentMs;
#endif
        }
        if (code >= 0) break;
        http.end();
        delay(backoffMs);
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
    } else if (code == HTTP_CODE_OK && http.getSize() >= 0 &&
               static_cast<uint32_t>(http.getSize()) <= range.last + 1) {
        // A .crc is asked for whole, and the card answers with a plain 200 and the file. A 200
        // for a file bigger than the range means it ignored the range: a failure.
        const String body = http.getString();
        reply.status = ProbeStatus::Hit;
        reply.signature.size = static_cast<uint64_t>(http.getSize());
        if (ezshare::keepsSignatureBytes(pattern) && body.length() > range.first) {
            reply.signature.bytes.assign(body.c_str() + range.first, body.length() - range.first);
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

struct CopyResult {
    const char* cause = "none";  // "done", "sd_short_write", "timeout" or "closed"
    int chunk = 0;               // on a short write: bytes handed to the SD file
    int written = 0;             // ...and bytes it accepted
};

// Copies the response body into `out` a sector at a time. HTTPClient::writeToStream gave up
// after the first sector on every large file, so this checks each write itself.
CopyResult copyBody(HTTPClient& http, fs::File& out) {
    WiFiClient* stream = http.getStreamPtr();
    int remaining = http.getSize();  // -1 when the card sends no Content-Length
    uint8_t buf[512];
    uint32_t lastData = millis();
    CopyResult r;
    while (remaining != 0) {
        const size_t avail = stream->available();
        if (avail == 0) {
            if (!stream->connected()) return r.cause = remaining < 0 ? "done" : "closed", r;
            if (millis() - lastData > kDownloadTimeoutMs) return r.cause = "timeout", r;
            delay(1);
            continue;
        }
        const size_t want = std::min(avail, remaining > 0 ? std::min(sizeof(buf), static_cast<size_t>(remaining)) : sizeof(buf));
        const size_t n = stream->readBytes(buf, want);
        const size_t w = out.write(buf, n);
        if (w != n) {
            r.cause = "sd_short_write";
            r.chunk = static_cast<int>(n);
            r.written = static_cast<int>(w);
            return r;
        }
        if (remaining > 0) remaining -= static_cast<int>(n);
        lastData = millis();
    }
    r.cause = "done";
    return r;
}

// Streams `name` into <name>.part, resuming with a Range request when a transfer stalls or
// comes up short, and renames it over the copy once it has all `total` bytes.
bool download(const Folder& folder, const std::string& name, uint64_t total, Run& run) {
    if (run.stopForTime()) return false;
    const uint32_t startMs = millis();
    const String path = folder.local + "/" + name.c_str();
    const String partPath = path + ".part";

    int code = -1;
    bool fromStart = true;
    for (int attempt = 0; attempt < kDownloadAttempts; ++attempt) {
        fs::File part = SDFS.open(partPath, fromStart ? "w" : "a");
        fromStart = false;
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
        const CopyResult copied = code == expected ? copyBody(http, part) : CopyResult{};
        const size_t got = part.size();
        part.close();
        http.end();

        if (code > 0 && code != expected) {
            // A resume the card didn't answer with 206: start the file over rather than give up.
            if (have == 0) break;
            fromStart = true;
            logEvent("warn", "download_restarted", {text("path", path), num("status", code)});
            continue;
        }
        if (got == total) {
            // FAT can't rename over an existing file. The puller keeps the previous copy meanwhile.
            // The rename has failed now and then right after a large download, so it gets a
            // second try before the error is logged with the card's own error code.
            SDFS.remove(path);
            bool renamed = SDFS.rename(partPath, path);
            if (!renamed) {
                delay(200);
                SDFS.remove(path);
                renamed = SDFS.rename(partPath, path);
            }
            if (!renamed) {
                logEvent("error", "sd_error", {text("op", "rename"), text("path", path),
                                               num("sd_error_code", sdFat()->sdErrorCode())});
                run.pending++;
                run.fail(SyncResult::SdError);
                return false;
            }
            metrics.filesDownloaded++;
            metrics.bytesDownloaded += total;
            logEvent("info", "copied", {text("path", path), num("bytes", static_cast<int64_t>(total)),
                                        num("ms", millis() - startMs)});
            return true;
        }
        logEvent("warn", "download_stalled",
                 {text("path", path), num("bytes", got), num("total", static_cast<int64_t>(total)),
                  text("cause", copied.cause), num("chunk", copied.chunk), num("written", copied.written),
                  num("heap", ESP.getFreeHeap())});
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

// The names of the files in a folder on the microSD.
std::vector<std::string> localNames(const String& path) {
    std::vector<std::string> names;
    fs::Dir dir = SDFS.openDir(path);
    while (dir.next()) {
        if (dir.isFile()) names.emplace_back(dir.fileName().c_str());
    }
    return names;
}

ezshare::Date localToday() {
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    return {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
}

constexpr const char* kSettingsListedPath = "/.bridge/settings-listed";

// SETTINGS holds about 40 files with three-letter names that rarely change, and finding the
// names takes about 1,000 probes. So the names are searched for once a day, or when the bridge
// has none; other runs re-check only the files it already has.
void syncSettings(Run& run) {
    const Folder settings{"..%5CSETTINGS", "/SETTINGS"};
    const std::string today = ezshare::folderName(localToday());
    std::vector<std::string> stems = ezshare::settingsStems(localNames(settings.local));
    String listedOn;
    if (fs::File f = SDFS.open(kSettingsListedPath, "r")) {
        listedOn = f.readStringUntil('\n');
        f.close();
    }
    const bool search = stems.empty() || listedOn != today.c_str();
    if (search) {
        stems.clear();
        const auto exists = [&](const std::string& p) { return probeExists(settings, p, run); };
        if (!ezshare::expand("", std::vector<std::string_view>(3, ezshare::kLetters), exists, stems)) return;
    }
    SDFS.mkdir(settings.local);
    const uint32_t failuresBefore = run.failures;
    for (const auto& stem : stems) {
        if (run.stopForTime()) return;
        syncFile(settings, stem + ".tgt", run);
        syncFile(settings, stem + ".log", run);
    }
    if (search && run.failures == failuresBefore) {
        if (fs::File f = SDFS.open(kSettingsListedPath, "w")) {
            f.print(today.c_str());
            f.close();
        }
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

    // A finished night is complete up to its newest file, so only newer names are searched for.
    // A staged one may have gaps where a copy failed, so it's searched whole.
    std::vector<std::string> stamps;
    std::string after;
    if (!staged) {
        if (const auto plan = ezshare::planKnownNight(localNames(folder.local))) {
            after = plan->after;
            stamps = plan->recheck;
        }
    }
    const auto exists = [&](const std::string& p) { return probeExists(folder, p, run); };
    if (!ezshare::expand(name + "_", ezshare::timeAlphabets(false), exists, stamps, after)) return;
    const std::string nextDay = ezshare::folderName(ezshare::addDays(night, 1));
    if (!ezshare::expand(nextDay + "_", ezshare::timeAlphabets(true), exists, stamps, after)) return;

    SDFS.mkdir(folder.local);
    for (const auto& stamp : stamps) {
        for (const char* type : kSessionTypes) {
            if (run.stopForTime()) return;
            syncFile(folder, stamp + "_" + type + ".edf", run);
        }
    }
    const bool failed = run.failures != failuresBefore;
    if (staged && !failed && !SDFS.rename(folder.local, finalPath)) {
        logEvent("error", "sd_error", {text("op", "rename"), text("path", finalPath)});
        run.fail(SyncResult::SdError);
    }
    // A finished night is searched only after its newest file, so a copy that failed inside it
    // would never be looked for again. Staging it again makes the next run search it whole. The
    // puller already has its files and keeps them.
    if (!staged && failed) {
        const String stagedPath = String("/DATALOG/.") + name.c_str();
        if (SDFS.rename(finalPath, stagedPath)) {
            logEvent("warn", "night_restaged", {text("path", finalPath)});
        } else {
            logEvent("error", "sd_error", {text("op", "rename"), text("path", finalPath)});
        }
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

constexpr const char* kBackfillPath = "/.bridge/backfilled";
constexpr const char* kRunCountPath = "/.bridge/runs";

// Counts runs across restarts, so each run's log lines share a number no other run uses.
uint32_t nextRunNumber() {
    uint32_t n = 0;
    if (fs::File f = SDFS.open(kRunCountPath, "r")) {
        n = f.readStringUntil('\n').toInt();
        f.close();
    }
    ++n;
    if (fs::File f = SDFS.open(kRunCountPath, "w")) {
        f.print(n);
        f.close();
    }
    return n;
}

// The newest night older than the lookback window that a run has already checked, along with
// every night before it.
std::optional<ezshare::Date> backfilledThrough() {
    fs::File f = SDFS.open(kBackfillPath, "r");
    if (!f) return std::nullopt;
    const String s = f.readStringUntil('\n');
    f.close();
    return ezshare::parseFolderName(std::string_view(s.c_str(), s.length()));
}

// Night folders newest first: the lookback window every run, then any older night the
// bridge hasn't finished or checked, back to the card's start date.
void syncDatalog(Run& run) {
    const ezshare::Date today = localToday();
    const ezshare::Date lookbackStart = ezshare::addDays(today, -LOOKBACK_DAYS);
    const ezshare::Date oldest = ezshare::backfillFloor(lookbackStart, cardStartDate(), backfilledThrough());
    const uint32_t failuresBefore = run.failures;

    SDFS.mkdir("/DATALOG");
    for (ezshare::Date d = today; ezshare::daysBetween(oldest, d) >= 0; d = ezshare::addDays(d, -1)) {
        const std::string name = ezshare::folderName(d);
        const bool exists = SDFS.exists(String("/DATALOG/") + name.c_str());
        if (!ezshare::shouldListDatalogFolder(name, today, LOOKBACK_DAYS, exists)) continue;
        if (run.stopForTime()) return;
        syncNight(d, run);
    }

    // Every older night was visited without a failure, so later runs needn't look again.
    if (run.failures == failuresBefore) {
        if (fs::File f = SDFS.open(kBackfillPath, "w")) {
            f.print(ezshare::folderName(ezshare::addDays(lookbackStart, -1)).c_str());
            f.close();
        }
    }
}

// SDFS's info64() multiplies cluster counts by the cluster size in 32 bits, so its sizes wrap on
// a card over 4 GB. The volume's own counts don't.
void updateSdFree() {
    auto* vol = sdFat()->vol();
    const int32_t freeClusters = vol->freeClusterCount();
    if (freeClusters < 0) return;
    metrics.sdFreeBytes = static_cast<uint64_t>(freeClusters) * vol->bytesPerCluster();
    metrics.sdFreeMeasured = true;
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
    if (metrics.sdOk) setLogRun(nextRunNumber());

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
              num("ezshare_rssi", metrics.ezshareRssi), num("home_rssi", WiFi.RSSI()),
              num("lookback_days", LOOKBACK_DAYS)});
    setLogRun(0);
    return run.result;
}
