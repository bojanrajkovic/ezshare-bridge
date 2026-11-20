#include "log.h"

#include <ESP8266WiFi.h>
#include <SDFS.h>
#include <WiFiUdp.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
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

uint32_t logRun = 0;

void setLogRun(uint32_t run) { logRun = run; }

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
    if (logRun != 0) {
        line += ",\"run\":";
        line += logRun;
    }
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

// Right after the bridge joins a network, the gateway's MAC address is unknown. lwIP holds only
// one packet per address it's still looking up, so a batch sent then loses all but its last
// line: the boot line and a run's sync line went missing that way. Look the gateway up first.
bool gatewayResolved() {
    ip4_addr_t gateway;
    gateway.addr = static_cast<uint32_t>(WiFi.gatewayIP());
    struct eth_addr* mac = nullptr;
    const ip4_addr_t* ip = nullptr;
    if (etharp_find_addr(netif_default, &gateway, &mac, &ip) >= 0) return true;
    etharp_request(netif_default, &gateway);
    for (int i = 0; i < 50; ++i) {
        delay(10);
        if (etharp_find_addr(netif_default, &gateway, &mac, &ip) >= 0) return true;
    }
    return false;
}

void shipLogs() {
    if (strlen(SYSLOG_HOST) == 0 || !metrics.sdOk || WiFi.status() != WL_CONNECTED) return;
    static IPAddress target;
    if (!target.isSet() && !WiFi.hostByName(SYSLOG_HOST, target)) return;
    if (!gatewayResolved()) return;  // the lines wait for the next call

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
        if (line.isEmpty()) {
            offset = f.position();
            continue;
        }
        const String ts = field(line, "ts");
        udp.beginPacket(target, SYSLOG_PORT);
        udp.printf("<%d>1 %s %s ezshare-bridge - - - ", priority(field(line, "level")), ts.isEmpty() ? "-" : ts.c_str(),
                   BRIDGE_HOSTNAME);
        udp.print(line);
        // The stack refuses packets when its buffers are full; resend this line on the next call.
        if (!udp.endPacket()) break;
        offset = f.position();
        delay(10);  // back-to-back packets overflow the Wi-Fi transmit queue and are dropped silently
    }
    f.close();
    writeShipped(offset);
}

fs::File openLog() { return SDFS.open(kLogPath, "r"); }
