#include "ezshare.h"

#include <cctype>
#include <cstdio>

namespace ezshare {

namespace {

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool allDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

int toInt(std::string_view s) {
    int v = 0;
    for (char c : s) v = v * 10 + (c - '0');
    return v;
}

bool validDate(const Date& d) { return d.month >= 1 && d.month <= 12 && d.day >= 1 && d.day <= 31; }

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil).
int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// The inverse of daysFromCivil (Howard Hinnant's civil_from_days).
Date civilFromDays(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    return Date{static_cast<int>(yoe + era * 400) + (m <= 2), m, d};
}

int64_t toDays(Date d) { return daysFromCivil(d.year, d.month, d.day); }

bool parseFolderDate(std::string_view name, Date& out) {
    if (name.size() != 8 || !allDigits(name)) return false;
    out = Date{toInt(name.substr(0, 4)), toInt(name.substr(4, 2)), toInt(name.substr(6, 2))};
    return validDate(out);
}

bool expandFrom(std::string& prefix, const std::vector<std::string_view>& alphabets, size_t level,
                const Probe& probe, std::vector<std::string>& found) {
    if (level == alphabets.size()) {
        found.push_back(prefix);
        return true;
    }
    for (char c : alphabets[level]) {
        prefix.push_back(c);
        const ProbeStatus s = probe(prefix + "*");
        bool ok = s != ProbeStatus::Failed;
        if (s == ProbeStatus::Hit) ok = expandFrom(prefix, alphabets, level + 1, probe, found);
        prefix.pop_back();
        if (!ok) return false;
    }
    return true;
}

}  // namespace

bool expand(const std::string& prefix, const std::vector<std::string_view>& alphabets, const Probe& probe,
            std::vector<std::string>& found) {
    std::string p = prefix;
    return expandFrom(p, alphabets, 0, probe, found);
}

std::vector<std::string_view> timeAlphabets(bool nextDay) {
    return {nextDay ? "01" : "12", kDigits, "012345", kDigits, "012345", kDigits};
}

Date addDays(Date d, int days) { return civilFromDays(toDays(d) + days); }

int daysBetween(Date from, Date to) { return static_cast<int>(toDays(to) - toDays(from)); }

std::optional<Date> parseFolderName(std::string_view name) {
    Date d{};
    if (!parseFolderDate(name, d)) return std::nullopt;
    return d;
}

Date backfillFloor(Date lookbackStart, std::optional<Date> cardStart, std::optional<Date> checkedThrough) {
    if (!cardStart || daysBetween(*cardStart, lookbackStart) <= 0) return lookbackStart;
    Date floor = *cardStart;
    if (checkedThrough && daysBetween(floor, *checkedThrough) >= 0) floor = addDays(*checkedThrough, 1);
    return daysBetween(floor, lookbackStart) < 0 ? lookbackStart : floor;
}

std::string folderName(Date d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", d.year, d.month, d.day);
    return buf;
}

std::optional<Date> parseEdfStartDate(std::string_view field) {
    if (field.size() != 8 || field[2] != '.' || field[5] != '.') return std::nullopt;
    const std::string_view dd = field.substr(0, 2), mm = field.substr(3, 2), yy = field.substr(6, 2);
    if (!allDigits(dd) || !allDigits(mm) || !allDigits(yy)) return std::nullopt;
    const int y = toInt(yy);
    const Date d{y >= 85 ? 1900 + y : 2000 + y, toInt(mm), toInt(dd)};
    if (!validDate(d)) return std::nullopt;
    return d;
}

bool shouldListDatalogFolder(std::string_view name, Date today, int lookbackDays, bool existsLocally) {
    Date folder{};
    if (!parseFolderDate(name, folder)) return false;
    return !existsLocally || daysBetween(folder, today) <= lookbackDays;
}

std::optional<uint64_t> contentRangeTotal(std::string_view header) {
    const size_t slash = header.rfind('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const std::string_view total = header.substr(slash + 1);
    if (!allDigits(total) || total.size() > 12) return std::nullopt;
    uint64_t v = 0;
    for (char c : total) v = v * 10 + static_cast<uint64_t>(c - '0');
    return v;
}

ByteRange signatureRange(std::string_view name) {
    if (endsWith(name, ".edf")) return {236, 243};
    if (endsWith(name, ".crc")) return {0, 7};
    return {0, 0};
}

bool sendsRange(std::string_view name) { return !endsWith(name, ".crc"); }

bool keepsSignatureBytes(std::string_view name) { return endsWith(name, ".edf") || endsWith(name, ".crc"); }

std::string crcCompanion(std::string_view name) {
    if (!endsWith(name, ".edf") && !endsWith(name, ".tgt")) return "";
    return std::string(name.substr(0, name.size() - 4)) + ".crc";
}

bool needsDownload(const std::optional<Signature>& local, const Signature& remote, bool companionChanged) {
    return !local || *local != remote || companionChanged;
}

bool isSafeName(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || endsWith(name, ".part")) return false;
    for (char c : name) {
        if (c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xf];
                    out += hex[c & 0xf];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

bool inWindow(int hour, int startHour, int endHour) {
    if (startHour == endHour) return true;
    if (startHour < endHour) return hour >= startHour && hour < endHour;
    return hour >= startHour || hour < endHour;
}

}  // namespace ezshare
