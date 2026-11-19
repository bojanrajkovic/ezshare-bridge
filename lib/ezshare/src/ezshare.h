// Parsing and download decisions for the ez Share Wi-Fi SD card's web listing.
//
// This file has no Arduino dependencies, so it builds and runs in the native test
// environment. The firmware does the HTTP requests and SD card writes.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace ezshare {

// Wall-clock time as the card reports it: the CPAP's local time, no zone.
struct Timestamp {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
};

struct File {
    std::string name;   // long name from the link text, e.g. "20261118_120004_EVE.edf"
    std::string query;  // download query exactly as the card sent it, e.g. "file=DATALOG%5C20261118%5C20261~1.EDF"
    uint32_t sizeKb;    // the card lists sizes rounded to KB
    Timestamp modified;
};

struct Directory {
    std::string name;   // e.g. "20261118"
    std::string query;  // listing query exactly as the card sent it, e.g. "dir=A:%5CDATALOG%5C20261118"
    Timestamp modified;
};

// A line with no link: the page header, a blank line, the closing tags.
struct NotAnEntry {};

// A line with a link the parser doesn't understand. The caller treats the listing as
// failed rather than skipping the entry, so an unexpected format can't silently drop data.
struct Malformed {};

using ParsedLine = std::variant<NotAnEntry, File, Directory, Malformed>;

// Parses one line of the <pre> block the card returns for /dir?dir=...
ParsedLine parseLine(std::string_view line);

struct Date {
    int year;
    int month;
    int day;
};

// Whether to list a DATALOG/<name> folder. Folders inside the lookback window are always
// listed, because the CPAP may still be writing to them. Older folders are listed only if
// the bridge has never copied them, so a new card's history is copied once.
// Names that aren't YYYYMMDD dates are never listed.
bool shouldListDatalogFolder(std::string_view name, Date today, int lookbackDays, bool existsLocally);

struct LocalFile {
    uint64_t sizeBytes;
    int64_t modifiedEpoch;
};

// Whether the bridge's copy is missing or out of date. The firmware sets each downloaded
// file's modification time from the listing, so a matching time plus a size within the
// card's KB rounding means the copy is current. FAT stores times in 2-second steps, so
// times within 2 seconds match.
bool needsDownload(const std::optional<LocalFile>& local, uint32_t remoteSizeKb, int64_t remoteEpoch);

// Whether a name from the card is safe to use as one path segment on the bridge's SD card
// and in its HTTP index: not empty, not "." or "..", no separators or control characters,
// and not ending in ".part" (the bridge's own in-progress suffix).
bool isSafeName(std::string_view name);

// Whether a byte count matches a size the card listed in KB.
bool sizeMatchesKb(uint64_t sizeBytes, uint32_t sizeKb);

// Escapes `s` for use inside a JSON string literal (without the surrounding quotes).
std::string jsonEscape(std::string_view s);

// Whether `hour` falls in [startHour, endHour). A window with startHour > endHour wraps
// past midnight. startHour == endHour means the window is always open.
bool inWindow(int hour, int startHour, int endHour);

}  // namespace ezshare
