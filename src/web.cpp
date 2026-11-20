#include "web.h"

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <SDFS.h>
#include <time.h>

#include "config.h"
#include "ezshare.h"
#include "log.h"
#include "state.h"

namespace {

ESP8266WebServer server(80);

constexpr const char* kFilesPrefix = "/files";

// Paths are relative to the microSD root. Hidden entries (the bridge's .bridge/ folder)
// and in-progress .part files are never served or listed.
bool isServable(const String& path) {
    if (path.indexOf("/.") >= 0 || path.endsWith(".part")) return false;
    return true;
}

// The microSD path a URL names: "/" for /files and /files/, "/a/b" for /files/a/b. Empty for any
// other URL, and for one that reaches outside the card's files or into a hidden or .part entry.
String servedPath(const String& url) {
    const String decoded = ESP8266WebServer::urlDecode(url);
    if (decoded == kFilesPrefix) return "/";
    if (!decoded.startsWith(String(kFilesPrefix) + "/")) return "";
    const String path = decoded.substring(strlen(kFilesPrefix));
    if (path.indexOf("..") >= 0 || !isServable(path)) return "";
    return path;
}

String httpDate(time_t t) {
    char buf[40];
    tm g{};
    gmtime_r(&t, &g);
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &g);
    return String(buf);
}

// Sent in chunks: DATALOG/ gains a line every night, so the page outgrows the heap in time.
void sendIndex(const String& path) {
    fs::Dir dir = SDFS.openDir(path);
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "<html><body><pre>\n");
    while (dir.next()) {
        const String name = dir.fileName();
        if (name.startsWith(".") || name.endsWith(".part")) continue;
        const String href = dir.isDirectory() ? name + "/" : name;
        server.sendContent("<a href=\"" + href + "\">" + href + "</a>\n");
    }
    server.sendContent("</pre></body></html>\n");
    server.sendContent("");
}

// A write gives up after this long without progress. The default 5 s cut downloads short on a
// lossy link, and the puller can't resume them.
constexpr uint32_t kWriteTimeoutMs = 30000;

// Writes `len` bytes of `f`, from where it's positioned, to the client.
void sendBytes(fs::File& f, uint32_t len) {
    uint8_t buf[512];
    while (len > 0) {
        const size_t n = f.read(buf, std::min<uint32_t>(sizeof(buf), len));
        if (n == 0 || server.client().write(buf, n) != n) return;
        len -= n;
    }
}

void sendFile(const String& path) {
    fs::File f = SDFS.open(path, "r");
    if (!f) {
        server.send(404, "text/plain", "not found\n");
        return;
    }
    server.sendHeader("Last-Modified", httpDate(f.getLastWrite()));
    // A client resumes a cut-off download with a Range request. Without one, rclone appended a
    // whole second copy of the file and threw both away.
    server.sendHeader("Accept-Ranges", "bytes");
    const String rangeHeader = server.header("Range");
    const auto range = ezshare::requestedRange(std::string_view(rangeHeader.c_str(), rangeHeader.length()), f.size());
    if (server.method() == HTTP_GET && range) {
        const uint32_t len = range->last - range->first + 1;
        f.seek(range->first);
        server.sendHeader("Content-Range", "bytes " + String(range->first) + "-" + String(range->last) + "/" +
                                               String(static_cast<uint32_t>(f.size())));
        server.setContentLength(len);
        server.client().setTimeout(kWriteTimeoutMs);
        server.send(206, "application/octet-stream", "");
        sendBytes(f, len);
    } else if (server.method() == HTTP_HEAD) {
        server.setContentLength(f.size());
        server.send(200, "application/octet-stream", "");
    } else {
        server.client().setTimeout(kWriteTimeoutMs);
        server.streamFile(f, "application/octet-stream");
    }
    f.close();
}

void handleFiles() {
    const String uri = server.uri();
    const String path = servedPath(uri);
    if (path.isEmpty()) {
        server.send(404, "text/plain", "not found\n");
        return;
    }
    if (!metrics.sdOk) {
        server.send(503, "text/plain", "sd card unavailable\n");
        return;
    }

    if (path.endsWith("/")) {
        const String dirPath = path.length() > 1 ? path.substring(0, path.length() - 1) : path;
        if (dirPath != "/" && !SDFS.exists(dirPath)) {
            server.send(404, "text/plain", "not found\n");
            return;
        }
        sendIndex(dirPath);
        return;
    }
    if (fs::File f = SDFS.open(path, "r")) {
        const bool isDir = f.isDirectory();
        f.close();
        if (isDir) {
            server.sendHeader("Location", uri + "/");
            server.send(301, "text/plain", "");
            return;
        }
    }
    sendFile(path);
}

void metric(String& out, const char* name, const char* type, const char* help) {
    out += "# HELP ";
    out += name;
    out += ' ';
    out += help;
    out += "\n# TYPE ";
    out += name;
    out += ' ';
    out += type;
    out += '\n';
}

void handleMetrics() {
    String out;
    out.reserve(2048);

    metric(out, "ezshare_bridge_sync_last_success_timestamp_seconds", "gauge",
           "Unix time of the last sync that finished with nothing pending.");
    out += "ezshare_bridge_sync_last_success_timestamp_seconds " + String(metrics.lastSuccessEpoch) + "\n";

    metric(out, "ezshare_bridge_sync_runs_total", "counter", "Sync runs by how they ended.");
    for (int i = 0; i < static_cast<int>(SyncResult::Count); ++i) {
        out += "ezshare_bridge_sync_runs_total{result=\"";
        out += resultLabel(static_cast<SyncResult>(i));
        out += "\"} " + String(metrics.runs[i]) + "\n";
    }

    metric(out, "ezshare_bridge_files_downloaded_total", "counter", "Files copied from the card.");
    out += "ezshare_bridge_files_downloaded_total " + String(metrics.filesDownloaded) + "\n";
    metric(out, "ezshare_bridge_bytes_downloaded_total", "counter", "Bytes copied from the card.");
    out += "ezshare_bridge_bytes_downloaded_total " + String(static_cast<unsigned long long>(metrics.bytesDownloaded)) + "\n";
    metric(out, "ezshare_bridge_pending_files", "gauge", "Files the last sync left to copy.");
    out += "ezshare_bridge_pending_files " + String(metrics.pendingFiles) + "\n";
    metric(out, "ezshare_bridge_probes_total", "counter", "Requests that asked the card whether a file exists.");
    out += "ezshare_bridge_probes_total " + String(metrics.probes) + "\n";
    metric(out, "ezshare_bridge_sync_duration_seconds", "gauge", "How long the last sync took.");
    out += "ezshare_bridge_sync_duration_seconds " + String(metrics.lastSyncSeconds) + "\n";

    metric(out, "ezshare_bridge_sd_ok", "gauge", "Whether the bridge's microSD mounted.");
    out += "ezshare_bridge_sd_ok " + String(metrics.sdOk ? 1 : 0) + "\n";
    if (metrics.sdFreeMeasured) {
        metric(out, "ezshare_bridge_sd_free_bytes", "gauge", "Free space on the bridge's microSD after the last sync.");
        out += "ezshare_bridge_sd_free_bytes " + String(static_cast<unsigned long long>(metrics.sdFreeBytes)) + "\n";
    }

    metric(out, "ezshare_bridge_wifi_rssi_dbm", "gauge", "Signal strength by network.");
    out += "ezshare_bridge_wifi_rssi_dbm{network=\"home\"} " + String(WiFi.RSSI()) + "\n";
    if (metrics.ezshareSeen) {
        out += "ezshare_bridge_wifi_rssi_dbm{network=\"ezshare\"} " + String(metrics.ezshareRssi) + "\n";
    }

    metric(out, "ezshare_bridge_uptime_seconds", "gauge", "Seconds since boot.");
    out += "ezshare_bridge_uptime_seconds " + String(millis() / 1000) + "\n";
    metric(out, "ezshare_bridge_free_heap_bytes", "gauge", "Free heap.");
    out += "ezshare_bridge_free_heap_bytes " + String(ESP.getFreeHeap()) + "\n";
    metric(out, "ezshare_bridge_build_info", "gauge", "Firmware version.");
    out += "ezshare_bridge_build_info{version=\"" BRIDGE_VERSION "\"} 1\n";

    server.send(200, "text/plain; version=0.0.4", out);
}

void handleLog() {
    fs::File f = openLog();
    if (!f) {
        server.send(404, "text/plain", "no log yet\n");
        return;
    }
    server.client().setTimeout(kWriteTimeoutMs);
    server.streamFile(f, "application/x-ndjson");
    f.close();
}

// Answers WebDAV PROPFIND, so rclone's webdav backend gets every file's size and date from one
// request per folder instead of a HEAD per file. The server takes unknown methods for GET, so
// this runs as a hook before routing. Read-only: every other DAV method falls through to the
// normal handlers.
ESP8266WebServer::ClientFuture handlePropfind(const String& method, const String& url, WiFiClient* client) {
    if (method != "PROPFIND") return ESP8266WebServer::CLIENT_REQUEST_CAN_CONTINUE;

    // The hook runs after the request line; the headers and body are still unread. A client that
    // stops sending mustn't stall the bridge, so reading them has one deadline.
    const uint32_t deadline = millis() + 2000;
    client->setTimeout(2000);
    bool depthZero = false;
    long bodyLength = 0;
    for (int lines = 0; lines < 32; ++lines) {
        String line = client->readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) break;
        line.toLowerCase();
        if (line.startsWith("depth:")) depthZero = line.endsWith("0");
        if (line.startsWith("content-length:")) bodyLength = line.substring(15).toInt();
    }
    // The body only names the properties wanted, and every reply carries the same few.
    while (bodyLength > 0 && client->connected() && static_cast<int32_t>(deadline - millis()) > 0) {
        if (client->read() >= 0) {
            --bodyLength;
        } else {
            delay(1);
        }
    }

    String path = servedPath(url);
    if (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);
    fs::File f;
    if (path.isEmpty() || !metrics.sdOk || !(path == "/" || (f = SDFS.open(path, "r")))) {
        client->print("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return ESP8266WebServer::CLIENT_MUST_STOP;
    }
    const bool isDir = path == "/" || f.isDirectory();
    const String base = String(kFilesPrefix) + (path == "/" ? "" : path);

    client->setTimeout(kWriteTimeoutMs);
    client->print("HTTP/1.1 207 Multi-Status\r\nContent-Type: application/xml; charset=utf-8\r\n"
                  "Connection: close\r\n\r\n<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                  "<D:multistatus xmlns:D=\"DAV:\">\n");
    if (!isDir) {
        client->print(ezshare::davResponse(base.c_str(), false, f.size(), httpDate(f.getLastWrite()).c_str()).c_str());
    } else {
        client->print(ezshare::davResponse((base + "/").c_str(), true, 0, "").c_str());
        if (!depthZero) {
            fs::Dir dir = SDFS.openDir(path);
            while (dir.next()) {
                const String name = dir.fileName();
                if (name.startsWith(".") || name.endsWith(".part")) continue;
                const String href = base + "/" + name + (dir.isDirectory() ? "/" : "");
                client->print(ezshare::davResponse(href.c_str(), dir.isDirectory(), dir.fileSize(),
                                                   httpDate(dir.fileTime()).c_str())
                                  .c_str());
            }
        }
    }
    client->print("</D:multistatus>\n");
    if (f) f.close();
    return ESP8266WebServer::CLIENT_MUST_STOP;
}

bool restartRequested = false;

// Restarting starts a sync right away. The OTA password guards it, so another device on the
// network can't keep the bridge restarting and syncing. Digest auth keeps the password off the
// wire.
void handleRestart() {
    if (!server.authenticate("bridge", OTA_PASSWORD)) return server.requestAuthentication(DIGEST_AUTH);
    logEvent("info", "restart_requested", {});
    server.send(202, "text/plain", "restarting\n");
    restartRequested = true;
}

}  // namespace

void startWeb() {
    server.collectHeaders("Range");
    server.addHook([](const String& method, const String& url, WiFiClient* client,
                      ESP8266WebServer::ContentTypeFunction) { return handlePropfind(method, url, client); });
    server.on("/metrics", HTTP_GET, handleMetrics);
    server.on("/log", HTTP_GET, handleLog);
    server.on("/restart", HTTP_POST, handleRestart);
    server.onNotFound(handleFiles);
    server.begin();
}

void handleWeb() {
    server.handleClient();
    if (restartRequested) {
        delay(200);  // let the reply reach the client
        ESP.restart();
    }
}
