#pragma once

#include <stdint.h>

// How a sync run ended. A run reports the first failure it hit.
enum class SyncResult : uint8_t {
    Ok,
    JoinFailed,      // couldn't join the card's network
    ProbeFailed,     // a probe got neither 206 nor 404, or timed out
    DownloadFailed,  // a download failed or came up short after its retries
    SdError,         // the bridge's own microSD failed
    TimeBudget,      // ran out of time with files left
    Count,
};

inline const char* resultLabel(SyncResult r) {
    switch (r) {
        case SyncResult::Ok: return "ok";
        case SyncResult::JoinFailed: return "join_failed";
        case SyncResult::ProbeFailed: return "probe_failed";
        case SyncResult::DownloadFailed: return "download_failed";
        case SyncResult::SdError: return "sd_error";
        case SyncResult::TimeBudget: return "time_budget";
        case SyncResult::Count: break;
    }
    return "unknown";
}

// Everything /metrics reports. Counters reset on reboot.
struct Metrics {
    uint32_t lastSuccessEpoch = 0;
    uint32_t runs[static_cast<int>(SyncResult::Count)] = {};
    uint32_t filesDownloaded = 0;
    uint64_t bytesDownloaded = 0;
    uint32_t pendingFiles = 0;
    uint32_t probes = 0;
    uint32_t lastSyncSeconds = 0;
    uint64_t sdFreeBytes = 0;
    bool sdFreeMeasured = false;  // free space costs a full FAT scan, so it's measured only after a sync
    bool sdOk = false;
    int32_t ezshareRssi = 0;
    bool ezshareSeen = false;
};

extern Metrics metrics;
