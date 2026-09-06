// hal::relay on the Teensy 4.1 / grblHAL V2.09 carrier.
//
// K1 (Hi-Z), K2 (Lo-Z) and K3 (bypass changeover) coils are driven from
// the carrier's relay-driver outputs (docs/HW-T41-PINMAP.md §5: pins 12,
// 11, 19). Every switch is two-pole — both line legs — from one driver
// output (CLAUDE.md "Balanced L-Network").
//
// Fail-safe mapping for K3: BYPASS is the DE-ENERGISED state, so a
// controller power loss, a brown-out during boot, or a firmware crash
// all leave the network out of circuit. "Engaged" (in circuit) energises
// the coil. init() therefore only has to drive the pin to the inactive
// level to satisfy invariant 2, and it runs before anything else in
// app::motion::init().
//
// K1 / K2 are mutually exclusive: switching side is break-before-make
// with a short dwell so both legs are never bridged. Not RF-hot by
// construction — set_side is refused under RF one layer up.
//
// kDriverActiveHigh: the carrier's relay drivers are open-collector
// stages that sink the coil when the Teensy pin is HIGH (grblHAL
// convention for its spindle / coolant outputs). Confirm on the bench
// at commissioning (PLAN.md M1b.2) and flip here if the stage inverts.

#ifdef TARGET_TEENSY41

#include "hal/hal.h"

#include <Arduino.h>

#include "hal/board/t41_v209.h"

namespace hal::relay {

namespace {

namespace board = hal::board::t41_v209;

constexpr bool     kDriverActiveHigh = true;
constexpr uint32_t kBreakBeforeMakeMs = 10;

constexpr uint8_t COIL_ON  = kDriverActiveHigh ? HIGH : LOW;
constexpr uint8_t COIL_OFF = kDriverActiveHigh ? LOW  : HIGH;

SideSel current_side   = SideSel::HiZ;
bool    bypass_engaged = true;

void coil(uint8_t pin, bool on) { digitalWrite(pin, on ? COIL_ON : COIL_OFF); }

} // namespace

void init() {
    // Drive K3 to its inactive (= BYPASS) level before configuring
    // anything else, so the pin never floats high through boot.
    digitalWrite(board::RELAY_K3_BYPASS, COIL_OFF);
    pinMode(board::RELAY_K3_BYPASS, OUTPUT);
    digitalWrite(board::RELAY_K3_BYPASS, COIL_OFF);
    bypass_engaged = true;

    pinMode(board::RELAY_K1_HIZ, OUTPUT);
    pinMode(board::RELAY_K2_LOZ, OUTPUT);
    coil(board::RELAY_K2_LOZ, false);
    coil(board::RELAY_K1_HIZ, true);
    current_side = SideSel::HiZ;
}

void set_side(SideSel s) {
    if (s == current_side) return;
    coil(board::RELAY_K1_HIZ, false);
    coil(board::RELAY_K2_LOZ, false);
    delay(kBreakBeforeMakeMs);
    coil(s == SideSel::HiZ ? board::RELAY_K1_HIZ : board::RELAY_K2_LOZ, true);
    current_side = s;
}

SideSel side() { return current_side; }

void set_bypass(bool on) {
    // on == bypass == coil de-energised.
    coil(board::RELAY_K3_BYPASS, !on);
    bypass_engaged = on;
}

bool bypass() { return bypass_engaged; }

} // namespace hal::relay

#endif // TARGET_TEENSY41
