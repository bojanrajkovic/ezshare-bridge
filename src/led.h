#pragma once

// The ESP's own LED (LD2 on GPIO2, lit when the pin is low). A timer drives it, so it keeps
// blinking while a sync blocks the main loop.
enum class LedPattern {
    Off,      // idle, last run fine
    Syncing,  // a short blink every second
    Failed,   // a double blink every 2 seconds, until the next run
};

void startLed();
void setLed(LedPattern pattern);
