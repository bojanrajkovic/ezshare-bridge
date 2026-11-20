#include "listing.h"

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

struct Numbers {
    long values[8];
    size_t count = 0;
    int kbIndex = -1;
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

bool sizeMatchesKb(uint64_t sizeBytes, uint32_t sizeKb) {
    const int64_t listed = static_cast<int64_t>(sizeKb) * 1024;
    return std::llabs(static_cast<int64_t>(sizeBytes) - listed) < 1024;
}

bool needsDownload(const std::optional<LocalFile>& local, uint32_t remoteSizeKb, int64_t remoteEpoch) {
    if (!local) return true;
    return std::llabs(local->modifiedEpoch - remoteEpoch) >= 2 || !sizeMatchesKb(local->sizeBytes, remoteSizeKb);
}

}  // namespace ezshare
