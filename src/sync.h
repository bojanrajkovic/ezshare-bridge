#pragma once

#include "state.h"

// Joins `ssid`, sending BRIDGE_HOSTNAME with the DHCP request. Returns false after
// `timeoutMs` without a connection.
bool connectWifi(const char* ssid, const char* password, uint32_t timeoutMs);

// Switches to the card's network, copies new and changed files to the microSD, and
// switches back to the home network. Updates `metrics` and returns how the run ended.
SyncResult runSync();
