#include "ezshare.h"

#include <cctype>
#include <cstdlib>

namespace ezshare {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// Reads the integers in `text` in order. The card pads single digits with spaces
// ("2024- 6- 7"), so separators are anything that isn't a digit.
struct Numbers {
    long values[8];
    size_t count = 0;
    // Index of the number directly followed by "KB", or -1.
    int kbIndex = -1;
    // Whether a number was followed by a size unit other than KB.
    bool otherUnit = false;
};

Numbers readNumbers(std::string_view text) {
    Numbers n;
    size_t i = 0;
    while (i < text.size() && n.count < 8) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            ++i;
            continue;
        }
        long value = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            value = value * 10 + (text[i] - '0');
            ++i;
        }
        std::string_view rest = text.substr(i);
        if (rest.substr(0, 2) == "KB") {
            n.kbIndex = static_cast<int>(n.count);
        } else if (rest.substr(0, 2) == "MB" || rest.substr(0, 2) == "GB" || rest.substr(0, 1) == "B") {
            n.otherUnit = true;
        }
        n.values[n.count++] = value;
    }
    return n;
}

bool validTimestamp(const Timestamp& t) {
    return t.year >= 1980 && t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 && t.hour >= 0 &&
           t.hour <= 23 && t.minute >= 0 && t.minute <= 59 && t.second >= 0 && t.second <= 59;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil).
int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool parseFolderDate(std::string_view name, Date& out) {
    if (name.size() != 8) return false;
    for (char c : name) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    auto num = [&](size_t pos, size_t len) {
        int v = 0;
        for (size_t i = pos; i < pos + len; ++i) v = v * 10 + (name[i] - '0');
        return v;
    };
    out = Date{num(0, 4), num(4, 2), num(6, 2)};
    return out.month >= 1 && out.month <= 12 && out.day >= 1 && out.day <= 31;
}

}  // namespace

ParsedLine parseLine(std::string_view line) {
    const size_t open = line.find("<a href=\"");
    if (open == std::string_view::npos) return NotAnEntry{};

    const size_t hrefStart = open + 9;
    const size_t hrefEnd = line.find('"', hrefStart);
    const size_t textStart = line.find('>', hrefEnd == std::string_view::npos ? hrefStart : hrefEnd);
    const size_t textEnd = line.find("</a>", textStart == std::string_view::npos ? hrefStart : textStart);
    if (hrefEnd == std::string_view::npos || textStart == std::string_view::npos ||
        textEnd == std::string_view::npos) {
        return Malformed{};
    }

    const std::string_view href = line.substr(hrefStart, hrefEnd - hrefStart);
    const std::string_view name = trim(line.substr(textStart + 1, textEnd - textStart - 1));
    const size_t question = href.find('?');
    if (name.empty() || question == std::string_view::npos) return Malformed{};
    const std::string_view path = href.substr(0, question);
    const std::string_view query = href.substr(question + 1);

    const std::string_view prefix = line.substr(0, open);
    const Numbers n = readNumbers(prefix);
    if (n.count < 6 || n.otherUnit) return Malformed{};
    const Timestamp modified{static_cast<int>(n.values[0]), static_cast<int>(n.values[1]),
                             static_cast<int>(n.values[2]), static_cast<int>(n.values[3]),
                             static_cast<int>(n.values[4]), static_cast<int>(n.values[5])};
    if (!validTimestamp(modified)) return Malformed{};

    if (endsWith(path, "download")) {
        if (n.kbIndex != 6) return Malformed{};
        return File{std::string(name), std::string(query), static_cast<uint32_t>(n.values[6]), modified};
    }
    if (endsWith(path, "dir")) {
        return Directory{std::string(name), std::string(query), modified};
    }
    return Malformed{};
}

bool shouldListDatalogFolder(std::string_view name, Date today, int lookbackDays, bool existsLocally) {
    Date folder{};
    if (!parseFolderDate(name, folder)) return false;
    if (!existsLocally) return true;
    const int64_t age = daysFromCivil(today.year, today.month, today.day) -
                        daysFromCivil(folder.year, folder.month, folder.day);
    return age <= lookbackDays;
}

bool sizeMatchesKb(uint64_t sizeBytes, uint32_t sizeKb) {
    const int64_t listed = static_cast<int64_t>(sizeKb) * 1024;
    return std::llabs(static_cast<int64_t>(sizeBytes) - listed) < 1024;
}

bool needsDownload(const std::optional<LocalFile>& local, uint32_t remoteSizeKb, int64_t remoteEpoch) {
    if (!local) return true;
    return std::llabs(local->modifiedEpoch - remoteEpoch) >= 2 || !sizeMatchesKb(local->sizeBytes, remoteSizeKb);
}

bool isSafeName(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || endsWith(name, ".part")) return false;
    for (char c : name) {
        if (c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

bool inWindow(int hour, int startHour, int endHour) {
    if (startHour == endHour) return true;
    if (startHour < endHour) return hour >= startHour && hour < endHour;
    return hour >= startHour || hour < endHour;
}

}  // namespace ezshare
