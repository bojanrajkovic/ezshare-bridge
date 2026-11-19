#include "log.h"

#include <SDFS.h>
#include <time.h>

#include "ezshare.h"
#include "state.h"

namespace {

constexpr const char* kLogPath = "/.bridge/log.txt";
constexpr const char* kOldLogPath = "/.bridge/log.1.txt";
constexpr const char* kShippedPath = "/.bridge/log.shipped";

// The log rotates once it's this big and every line has been shipped...
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

bool openUnshipped(fs::File& file, uint32_t& end) {
    file = SDFS.open(kLogPath, "r");
    if (!file) return false;
    end = static_cast<uint32_t>(file.size());
    uint32_t start = readShipped();
    if (start > end) start = 0;  // the file was replaced by hand
    file.seek(start);
    return true;
}

bool ackShipped(uint32_t offset) {
    fs::File f = SDFS.open(kLogPath, "r");
    const uint32_t size = f ? static_cast<uint32_t>(f.size()) : 0;
    if (f) f.close();
    if (offset > size || offset < readShipped()) return false;
    return writeShipped(offset);
}

fs::File openLog() { return SDFS.open(kLogPath, "r"); }
