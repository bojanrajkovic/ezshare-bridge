// ez Share bridge: copies the CPAP's data from an ez Share Wi-Fi SD card to a microSD, and
// serves it over HTTP for a puller on the home network. See README.md.
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <SDFS.h>
#include <time.h>

#include "config.h"
#include "ezshare.h"
#include "led.h"
#include "log.h"
#include "state.h"
#include "sync.h"
#include "web.h"

Metrics metrics;

namespace {

// 0 means no run yet. Set from millis() with the low bit forced on, so it's never 0.
uint32_t lastSyncAttemptMs = 0;
uint32_t lastShipMs = 0;
constexpr uint32_t kShipIntervalMs = 10000;

void startMdns() {
    MDNS.end();
    if (MDNS.begin(BRIDGE_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        MDNS.enableArduino(8266, true);
    }
}

bool timeIsSet() { return time(nullptr) > 1700000000; }

bool syncDue() {
    if (!timeIsSet()) return false;
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    if (!ezshare::inWindow(local.tm_hour, SYNC_WINDOW_START_HOUR, SYNC_WINDOW_END_HOUR)) return false;
    return lastSyncAttemptMs == 0 || millis() - lastSyncAttemptMs >= SYNC_INTERVAL_MINUTES * 60000UL;
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.printf("\nezshare-bridge %s\n", BRIDGE_VERSION);
    startLed();

    SDFS.setConfig(SDFSConfig(SD_CS_PIN, SPI_HALF_SPEED));
    metrics.sdOk = SDFS.begin();
    if (!metrics.sdOk) Serial.println("microSD did not mount");

    WiFi.persistent(false);  // credentials are compiled in; don't wear the flash rewriting them
    WiFi.mode(WIFI_STA);
    // Modem sleep turns the radio off between beacons. On a weak link it drops and delays enough
    // packets to cut file downloads short, and the bridge runs on USB power anyway.
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);
    while (!connectWifi(HOME_WIFI_SSID, HOME_WIFI_PASSWORD, JOIN_TIMEOUT_SECONDS * 1000UL)) {
        Serial.println("home network: retrying");
    }
    configTime(TZ_STRING, NTP_SERVER);
    // Give NTP a moment so the boot line carries a real timestamp; "up" orders it regardless.
    for (int i = 0; i < 50 && time(nullptr) < 1700000000; ++i) delay(100);
    logEvent("info", "boot",
             {text("version", BRIDGE_VERSION), text("reset_reason", ESP.getResetReason()),
              text("ip", WiFi.localIP().toString()), num("sd_ok", metrics.sdOk ? 1 : 0),
              num("lookback_days", LOOKBACK_DAYS)});

    ArduinoOTA.setHostname(BRIDGE_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.begin(false);  // startMdns() announces the OTA service
    startMdns();
    startWeb();
}

void loop() {
    handleWeb();
    ArduinoOTA.handle();
    MDNS.update();

    if (millis() - lastShipMs >= kShipIntervalMs) {
        lastShipMs = millis();
        shipLogs();
    }

    if (!syncDue()) return;
    lastSyncAttemptMs = millis() | 1;
    setLed(LedPattern::Syncing);
    const SyncResult result = runSync();
    // A run that ran out of time isn't a failure; the next one carries on.
    setLed(result == SyncResult::Ok || result == SyncResult::TimeBudget ? LedPattern::Off : LedPattern::Failed);
    startMdns();  // the responder doesn't survive leaving the home network
    shipLogs();   // send what the run logged while it was on the card's network
    lastShipMs = millis();
}
