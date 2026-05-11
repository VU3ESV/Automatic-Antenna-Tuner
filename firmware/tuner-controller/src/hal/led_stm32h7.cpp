#ifdef TARGET_STM32H743

#include "hal/hal.h"
#include <Arduino.h>

namespace hal::led {

static constexpr int PIN = LED_BUILTIN; // LD1 (PB0) on Nucleo-H743ZI

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
