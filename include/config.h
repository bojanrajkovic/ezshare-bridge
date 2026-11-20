// Build-time settings. Credentials come from secrets.h; everything else has a default that
// secrets.h (or -D build flags) can override.
#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h is missing: copy include/secrets.h.example or run `mise run secrets`"
#endif

#ifndef BRIDGE_HOSTNAME
#define BRIDGE_HOSTNAME "ezshare-bridge"
#endif

#ifndef BRIDGE_VERSION
#define BRIDGE_VERSION "dev"
#endif

// The card always serves on this address on its own network.
#ifndef EZSHARE_HOST
#define EZSHARE_HOST "192.168.4.1"
#endif

// POSIX TZ string for the CPAP's clock. DATALOG folder names and the card's timestamps
// are in this zone.
#ifndef TZ_STRING
#define TZ_STRING "EST5EDT,M3.2.0,M11.1.0"
#endif

#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif

// Sync runs every SYNC_INTERVAL_MINUTES, only between these local hours, so the bridge
// stays off the card while the CPAP is recording at night.
#ifndef SYNC_INTERVAL_MINUTES
#define SYNC_INTERVAL_MINUTES 60
#endif
#ifndef SYNC_WINDOW_START_HOUR
#define SYNC_WINDOW_START_HOUR 9
#endif
#ifndef SYNC_WINDOW_END_HOUR
#define SYNC_WINDOW_END_HOUR 18
#endif

// DATALOG folders this many days old or newer are re-listed every run. Only the newest night
// changes once the CPAP is off, and re-listing a night costs about 2,000 probes.
#ifndef LOOKBACK_DAYS
#define LOOKBACK_DAYS 2
#endif

// After this long on the card's network, the bridge goes home even with work left. It's long
// enough for a night's copy (10-20 MB at about 5 KB/s); it exists so a hung run can't keep the
// bridge off the home network, where nothing can reach it.
#ifndef SYNC_TIME_BUDGET_SECONDS
#define SYNC_TIME_BUDGET_SECONDS 5400
#endif

#ifndef JOIN_TIMEOUT_SECONDS
#define JOIN_TIMEOUT_SECONDS 30
#endif

// FYSETC SD-WIFI (non-Pro): microSD on the hardware SPI bus (SCK 14, MISO 12, MOSI 13).
// Syslog receiver for log lines (RFC 5424 over UDP). Empty turns shipping off.
#ifndef SYSLOG_HOST
#define SYSLOG_HOST ""
#endif
#ifndef SYSLOG_PORT
#define SYSLOG_PORT 514
#endif

#ifndef SD_CS_PIN
#define SD_CS_PIN 4
#endif
