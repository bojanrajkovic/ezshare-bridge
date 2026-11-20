#include "led.h"

#include <Arduino.h>
#include <Ticker.h>

namespace {

constexpr uint8_t kLedPin = 2;
constexpr uint32_t kTickMs = 100;
constexpr uint8_t kTicksPerCycle = 20;

Ticker ticker;
volatile uint32_t litTicks = 0;  // bit n set: lit during tick n of each 2-second cycle
uint8_t tick = 0;

void step() {
    digitalWrite(kLedPin, (litTicks >> tick) & 1 ? LOW : HIGH);
    tick = (tick + 1) % kTicksPerCycle;
}

}  // namespace

void startLed() {
    pinMode(kLedPin, OUTPUT);
    digitalWrite(kLedPin, HIGH);
    ticker.attach_ms(kTickMs, step);
}

void setLed(LedPattern pattern) {
    switch (pattern) {
        case LedPattern::Off: litTicks = 0; break;
        case LedPattern::Syncing: litTicks = (1u << 0) | (1u << 10); break;
        case LedPattern::Failed: litTicks = (1u << 0) | (1u << 2); break;
    }
}
