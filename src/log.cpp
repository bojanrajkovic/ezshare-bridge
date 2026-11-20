#include "log.h"

#include <ESP8266WiFi.h>
#include <SDFS.h>
#include <WiFiUdp.h>
#include <time.h>

#include "config.h"
#include "ezshare.h"
#include "state.h"

namespace {

constexpr const char* kLogPath = "/.bridge/log.txt";
constexpr const char* kOldLogPath = "/.bridge/log.1.txt";
constexpr const char* kShippedPath = "/.bridge/log.shipped";

// The log rotates once it's this big and every line has been shipped to syslog...
constexpr size_t kRotateBytes = 256 * 1024;
// ...or at this size regardless, if shipping has stopped.
constexpr size_t kHardCapBytes = 1024 * 1024;

uint32_t readShipped() {
    fs::File f = SDFS.open(kShippedPath, "r");
    if (!f) return 0;
    const uint32_t offset = static_cast<uint32_t>(f.readString().toInt());
    f.close();
    return offset;
}

bool writeShipped(uint32_t offset) {
    fs::File f = SDFS.open(kShippedPath, "w");
    if (!f) return false;
    f.print(offset);
    f.close();
    return true;
}

void maybeRotate(size_t size) {
    if (size < kRotateBytes) return;
    if (size < kHardCapBytes && readShipped() < size) return;
    SDFS.remove(kOldLogPath);
    SDFS.rename(kLogPath, kOldLogPath);
    writeShipped(0);
}

String isoNow() {
    const time_t now = time(nullptr);
    if (now < 1700000000) return "";  // NTP hasn't answered yet; "up" still orders the line
    char buf[24];
    tm g{};
    gmtime_r(&now, &g);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &g);
    return String(buf);
}

String escape(const String& s) { return String(ezshare::jsonEscape(std::string_view(s.c_str(), s.length())).c_str()); }

}  // namespace

void logEvent(const char* level, const char* event, std::initializer_list<LogField> fields) {
    String line;
    line.reserve(160);
    line += "{\"ts\":\"";
    line += isoNow();
    line += "\",\"up\":";
    line += millis() / 1000;
    line += ",\"level\":\"";
    line += level;
    line += "\",\"logger\":\"ezshare-bridge\",\"event\":\"";
    line += event;
    line += '"';
    for (const auto& f : fields) {
        line += ",\"";
        line += f.key;
        line += "\":";
        if (f.quoted) {
            line += '"';
            line += escape(f.value);
            line += '"';
        } else {
            line += f.value;
        }
    }
    line += "}\n";
    Serial.print(line);

    if (!metrics.sdOk) return;
    SDFS.mkdir("/.bridge");
    fs::File f = SDFS.open(kLogPath, "a");
    if (!f) return;
    f.print(line);
    const size_t size = f.size();
    f.close();
    maybeRotate(size);
}

namespace {

// The value of "key":"..." in one of our own log lines, which never escape these fields.
String field(const String& line, const char* key) {
    const String marker = String("\"") + key + "\":\"";
    const int start = line.indexOf(marker);
    if (start < 0) return "";
    const int from = start + marker.length();
    const int end = line.indexOf('"', from);
    return end < 0 ? "" : line.substring(from, end);
}

// RFC 5424 severity for our level names; facility is local0 (16).
int priority(const String& level) {
    int severity = 6;  // info
    if (level == "debug") severity = 7;
    else if (level == "warn") severity = 4;
    else if (level == "error") severity = 3;
    else if (level == "fatal") severity = 2;
    return 16 * 8 + severity;
}

}  // namespace

void shipLogs() {
    if (strlen(SYSLOG_HOST) == 0 || !metrics.sdOk || WiFi.status() != WL_CONNECTED) return;
    static IPAddress target;
    if (!target.isSet() && !WiFi.hostByName(SYSLOG_HOST, target)) return;

    fs::File f = SDFS.open(kLogPath, "r");
    if (!f) return;
    uint32_t offset = readShipped();
    if (offset > f.size()) offset = 0;  // the file was replaced by hand
    if (offset == f.size()) {
        f.close();
        return;
    }
    f.seek(offset);

    WiFiUDP udp;
    // A batch per call keeps the web server responsive while a long backlog drains.
    for (int sent = 0; sent < 50 && f.available(); ++sent) {
        const String line = f.readStringUntil('\n');  // logEvent writes whole lines, so each ends in \n
        offset = f.position();
        if (line.isEmpty()) continue;
        const String ts = field(line, "ts");
        udp.beginPacket(target, SYSLOG_PORT);
        udp.printf("<%d>1 %s %s ezshare-bridge - - - ", priority(field(line, "level")), ts.isEmpty() ? "-" : ts.c_str(),
                   BRIDGE_HOSTNAME);
        udp.print(line);
        udp.endPacket();
    }
    f.close();
    writeShipped(offset);
}

fs::File openLog() { return SDFS.open(kLogPath, "r"); }
