#pragma once

#include <stdint.h>

// How a sync run ended. A run reports the first failure it hit.
enum class SyncResult : uint8_t {
    Ok,
    JoinFailed,      // couldn't join the card's network
    ListingFailed,   // a /dir request failed or returned a line the parser rejects
    DownloadFailed,  // a file request failed or its size didn't match
    SdError,         // the bridge's own microSD failed
    TimeBudget,      // ran out of time with files left
    Count,
};

inline const char* resultLabel(SyncResult r) {
    switch (r) {
        case SyncResult::Ok: return "ok";
        case SyncResult::JoinFailed: return "join_failed";
        case SyncResult::ListingFailed: return "listing_failed";
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
    uint64_t sdFreeBytes = 0;
    bool sdOk = false;
    int32_t ezshareRssi = 0;
    bool ezshareSeen = false;
};

extern Metrics metrics;
