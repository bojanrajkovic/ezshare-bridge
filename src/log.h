#pragma once

#include <Arduino.h>
#include <FS.h>

#include <initializer_list>

// One key/value on a log line. Text values are quoted and escaped; numbers are written as-is.
struct LogField {
    const char* key;
    String value;
    bool quoted;
};

inline LogField text(const char* key, const String& value) { return {key, value, true}; }
inline LogField num(const char* key, int64_t value) { return {key, String(static_cast<double>(value), 0), false}; }

// Writes one JSON line to the serial console and appends it to /.bridge/log.txt on the
// microSD. `level` is one of debug, info, warn, error, fatal.
void logEvent(const char* level, const char* event, std::initializer_list<LogField> fields = {});

// Opens the current log file positioned at the first line not yet acknowledged, and sets
// `end` to the file's length. Returns false if there's no log file.
bool openUnshipped(fs::File& file, uint32_t& end);

// Records that every line before `offset` has been shipped. Returns false if `offset` is
// before the last acknowledged offset or past the end of the file.
bool ackShipped(uint32_t offset);

// Opens the current log file for reading whole.
fs::File openLog();
