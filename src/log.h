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
inline LogField num(const char* key, int64_t value) { return {key, String(static_cast<long long>(value)), false}; }

// Writes one JSON line to the serial console and appends it to /.bridge/log.txt on the
// microSD. `level` is one of debug, info, warn, error, fatal.
void logEvent(const char* level, const char* event, std::initializer_list<LogField> fields = {});

// Tags every line logged from now on with "run": n, so a run's lines can be pulled out
// together. 0 stops tagging.
void setLogRun(uint32_t run);

// Sends log lines written since the last call to SYSLOG_HOST as RFC 5424 syslog over UDP,
// up to 50 per call, and records how far it got. Does nothing when SYSLOG_HOST is empty or
// the bridge is off the home network; the lines wait in the file until the next call.
// UDP has no acknowledgement, so a datagram lost on the network is gone from the receiver
// but stays in the file.
void shipLogs();

// Opens the current log file for reading whole.
fs::File openLog();
