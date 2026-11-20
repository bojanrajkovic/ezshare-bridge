#include "web.h"

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <SDFS.h>
#include <time.h>

#include "config.h"
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

void sendFile(const String& path) {
    fs::File f = SDFS.open(path, "r");
    if (!f) {
        server.send(404, "text/plain", "not found\n");
        return;
    }
    server.sendHeader("Last-Modified", httpDate(f.getLastWrite()));
    if (server.method() == HTTP_HEAD) {
        server.setContentLength(f.size());
        server.send(200, "application/octet-stream", "");
    } else {
        server.streamFile(f, "application/octet-stream");
    }
    f.close();
}

void handleFiles() {
    const String uri = server.uri();
    if (!uri.startsWith(kFilesPrefix)) {
        server.send(404, "text/plain", "not found\n");
        return;
    }
    String path = ESP8266WebServer::urlDecode(uri.substring(strlen(kFilesPrefix)));
    if (path.isEmpty()) path = "/";
    if (path.indexOf("..") >= 0 || !isServable(path)) {
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
    metric(out, "ezshare_bridge_sd_free_bytes", "gauge", "Free space on the bridge's microSD after the last sync.");
    out += "ezshare_bridge_sd_free_bytes " + String(static_cast<unsigned long long>(metrics.sdFreeBytes)) + "\n";

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
    server.streamFile(f, "application/x-ndjson");
    f.close();
}

// Lines not yet acknowledged, for a shipper that prints them and then POSTs /log/ack with
// the X-Log-End value. A shipper that dies before acknowledging gets the same lines again.
void handleUnshipped() {
    fs::File f;
    uint32_t end = 0;
    if (!openUnshipped(f, end)) {
        server.sendHeader("X-Log-End", "0");
        server.send(200, "application/x-ndjson", "");
        return;
    }
    server.sendHeader("X-Log-End", String(end));
    server.setContentLength(end - f.position());
    server.send(200, "application/x-ndjson", "");
    server.client().write(f);
    f.close();
}

void handleAck() {
    const String arg = server.arg("offset");
    char* rest = nullptr;
    const unsigned long offset = strtoul(arg.c_str(), &rest, 10);
    if (arg.isEmpty() || *rest != '\0' || !ackShipped(static_cast<uint32_t>(offset))) {
        server.send(400, "text/plain", "offset must be between the last acknowledged offset and the end of the log\n");
        return;
    }
    server.send(204);
}

}  // namespace

void startWeb() {
    server.on("/metrics", HTTP_GET, handleMetrics);
    server.on("/log", HTTP_GET, handleLog);
    server.on("/log/unshipped", HTTP_GET, handleUnshipped);
    server.on("/log/ack", HTTP_POST, handleAck);
    server.onNotFound(handleFiles);
    server.begin();
}

void handleWeb() { server.handleClient(); }
