// File discovery and download decisions for the ez Share Wi-Fi SD card.
//
// The card has no listing endpoint. Its /download?fname=<pattern> accepts `*` and `?`
// wildcards and answers 206 or 404, so the bridge finds files by probing patterns.
//
// This file has no Arduino dependencies, so it builds and runs in the native test
// environment. The firmware does the HTTP requests and SD card writes.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ezshare {

// How the card answered one probe: 206 (a file matches), 404 (none does), or anything
// else, including a timeout.
enum class ProbeStatus { Hit, Miss, Failed };

using Probe = std::function<ProbeStatus(const std::string& pattern)>;

inline constexpr std::string_view kDigits = "0123456789";
inline constexpr std::string_view kLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

// Finds every string prefix + c1..cn, with each ci from alphabets[i], for which a file
// matches. Tries "<prefix><c1>*", then extends only the hits, so a miss prunes its whole
// subtree. Appends the results to `found` in alphabet order. Returns false as soon as a
// probe fails.
//
// With `after`, a full result-length string, only results that sort after it are searched
// for: while a guess still matches after's leading characters, characters below after's are
// skipped. A prefix that sorts after `after` is searched whole; one that sorts before it isn't
// searched at all.
bool expand(const std::string& prefix, const std::vector<std::string_view>& alphabets, const Probe& probe,
            std::vector<std::string>& found, std::string_view after = {});

// What a run must look at in a night folder the bridge already has whole.
struct KnownNight {
    std::string after;                 // the newest session time copied, "YYYYMMDD_HHMMSS"
    std::vector<std::string> recheck;  // session times whose files can still change, oldest first
};

// Plans a run over a complete night folder from the names already copied. The CPAP names a
// session's files by its start time, so a new file always sorts after every copied one: only
// names after `after` need searching. Of the copied files, only the night's EVE and CSL (which
// grow all night) and the newest session can still change. Returns nullopt when no name is a
// session file, so the caller searches the folder whole.
std::optional<KnownNight> planKnownNight(const std::vector<std::string>& names);

// Alphabets for the HHMMSS part of a DATALOG file name. A night folder holds sessions that
// start after noon on its date and before noon the next day, so names with the folder's
// date have hours 1x-2x and names with the next date have 0x-1x.
std::vector<std::string_view> timeAlphabets(bool nextDay);

struct Date {
    int year;
    int month;
    int day;
};

Date addDays(Date d, int days);

// Days from `from` to `to`; negative when `to` is earlier.
int daysBetween(Date from, Date to);

// "YYYYMMDD", the DATALOG folder name and file name prefix for `d`.
std::string folderName(Date d);

// The date in a "YYYYMMDD" folder name, or nullopt if it isn't one.
std::optional<Date> parseFolderName(std::string_view name);

// The oldest night folder a run visits. Nights in the lookback window (from `lookbackStart`)
// are always visited; older ones back to `cardStart`, the first day the card has data for,
// except those already checked: a run that visits every older night without a failure records
// the newest of them as `checkedThrough`, and later runs start the day after it. Without that,
// nights the card doesn't have would be probed again every run.
Date backfillFloor(Date lookbackStart, std::optional<Date> cardStart, std::optional<Date> checkedThrough);

// Reads an EDF header's start date, "dd.mm.yy" at bytes 168-175. EDF years 85-99 are
// 1985-1999 and 00-84 are 2000-2084.
std::optional<Date> parseEdfStartDate(std::string_view field);

// Whether to probe a DATALOG/<name> folder. Folders inside the lookback window are always
// probed, because the CPAP may still be writing to them. Older folders are probed only if
// the bridge has never copied them, so a new card's history is copied once.
// Names that aren't YYYYMMDD dates are never probed.
bool shouldListDatalogFolder(std::string_view name, Date today, int lookbackDays, bool existsLocally);

// The total size from a Content-Range header, "bytes a-b/<total>".
std::optional<uint64_t> contentRangeTotal(std::string_view header);

struct ByteRange {
    uint32_t first;
    uint32_t last;
};

// The bytes a probe for `name` requests. They identify a version of the file together with
// its size: an .edf's record count (bytes 236-243), which the CPAP can rewrite on close
// without changing the size, and a .crc's whole content. Other files need only the size.
ByteRange signatureRange(std::string_view name);

// Whether a probe for `name` sends signatureRange(name) as a Range header. A .crc probe asks
// for the whole file instead: its range runs past the end of a 4-byte .crc, and the card then
// promises 8 bytes, sends 4, and keeps the download slot busy until it loses power.
bool sendsRange(std::string_view name);

// Whether the bytes at signatureRange(name) are part of the signature (.edf and .crc).
bool keepsSignatureBytes(std::string_view name);

// A file's size plus, for .edf and .crc, the bytes at signatureRange().
struct Signature {
    uint64_t size;
    std::string bytes;

    bool operator==(const Signature& o) const { return size == o.size && bytes == o.bytes; }
    bool operator!=(const Signature& o) const { return !(*this == o); }
};

// The .crc that checksums `name` (for .edf and .tgt), or "" if it has none.
std::string crcCompanion(std::string_view name);

// Whether the bridge's copy is missing or out of date. A file whose .crc changed is
// downloaded too, since the .crc is the CPAP's own record of the content.
bool needsDownload(const std::optional<Signature>& local, const Signature& remote, bool companionChanged);

// Whether a name is safe to use as one path segment on the bridge's SD card and in its
// HTTP index: not empty, not "." or "..", no separators or control characters, and not
// ending in ".part" (the bridge's own in-progress suffix).
bool isSafeName(std::string_view name);

// Escapes `s` for use inside a JSON string literal (without the surrounding quotes).
std::string jsonEscape(std::string_view s);

// Whether `hour` falls in [startHour, endHour). A window with startHour > endHour wraps
// past midnight. startHour == endHour means the window is always open.
bool inWindow(int hour, int startHour, int endHour);

}  // namespace ezshare
