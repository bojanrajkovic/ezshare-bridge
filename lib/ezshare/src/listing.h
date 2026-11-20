// The /dir HTML listing parser, kept until the firmware moves to wildcard probes.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace ezshare {

struct Timestamp {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
};

struct File {
    std::string name;
    std::string query;
    uint32_t sizeKb;
    Timestamp modified;
};

struct Directory {
    std::string name;
    std::string query;
    Timestamp modified;
};

struct NotAnEntry {};
struct Malformed {};

using ParsedLine = std::variant<NotAnEntry, File, Directory, Malformed>;

ParsedLine parseLine(std::string_view line);

struct LocalFile {
    uint64_t sizeBytes;
    int64_t modifiedEpoch;
};

bool needsDownload(const std::optional<LocalFile>& local, uint32_t remoteSizeKb, int64_t remoteEpoch);

bool sizeMatchesKb(uint64_t sizeBytes, uint32_t sizeKb);

}  // namespace ezshare
