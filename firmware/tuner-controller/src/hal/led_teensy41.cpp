#ifdef TARGET_TEENSY41

#include "hal/hal.h"
#include <Arduino.h>

namespace hal::led {

static constexpr int PIN = LED_BUILTIN; // pin 13 on Teensy 4.1

void init() {
    pinMode(PIN, OUTPUT);
    digitalWrite(PIN, LOW);
}

void set(bool on) {
    digitalWrite(PIN, on ? HIGH : LOW);
}

void toggle() {
    digitalWrite(PIN, !digitalRead(PIN));
}

} // namespace hal::led

#endif
