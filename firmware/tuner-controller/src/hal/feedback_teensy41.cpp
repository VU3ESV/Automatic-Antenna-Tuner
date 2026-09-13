// hal::feedback on the Teensy 4.1 / grblHAL V2.09 carrier: the iHSS60 PED
// (arrive position) and ALM (alarm) opto outputs on carrier opto inputs —
// pins and polarity in hal/board/t41_v209.h, wiring in
// docs/HW-T41-PINMAP.md §2.2. Levels only: whether the signals are wired
// and what they mean for motion is app::motion's (off by default).

#ifdef TARGET_TEENSY41

#include "hal/hal.h"

#include <Arduino.h>

#include "hal/board/t41_v209.h"

namespace hal::feedback {

namespace {

namespace board = hal::board::t41_v209;

static_assert(sizeof(board::FEEDBACK_PED) / sizeof(board::FEEDBACK_PED[0]) >= kMaxAxes,
              "one PED input per tuner axis");

bool input_active(uint8_t pin, bool active_low) {
    const int level = digitalRead(pin);
    return active_low ? (level == LOW) : (level == HIGH);
}

} // namespace

void init() {
    for (uint8_t a = 0; a < kMaxAxes; a++) pinMode(board::FEEDBACK_PED[a], INPUT_PULLUP);
    pinMode(board::FEEDBACK_ALM, INPUT_PULLUP);
}

bool arrived(Axis a) { return a < kMaxAxes && input_active(board::FEEDBACK_PED[a], board::PED_ACTIVE_LOW); }
bool alarm()         { return input_active(board::FEEDBACK_ALM, board::ALM_ACTIVE_LOW); }

} // namespace hal::feedback

#endif // TARGET_TEENSY41
