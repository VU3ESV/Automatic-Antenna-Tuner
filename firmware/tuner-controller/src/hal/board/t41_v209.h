#pragma once

// Board pin map: Teensy 4.1 mounted in Phil Barrett's grblHAL-teensy-4.x
// V2.09 carrier (T41E5XBB / T41U5XBB SKUs). Pin numbers are factual
// data extracted from upstream `T41U5XBB_map.h` (grblHAL/iMXRT1062) and
// re-typeset here; see docs/HW-T41-PINMAP.md for the full per-pin
// allocation table and the per-topology element assignment.
//
// This header is **pin map + signal polarity + driver-electrical
// defaults only — no logic**. Application code reaches axes and relays
// through hal::Axis / hal::relay (hal/hal.h) and never sees these pin
// numbers directly. Real-driver HAL implementations
// (motor_teensy41.cpp, relay_teensy41.cpp, encoder_teensy41.cpp —
// landing in M1b.2) translate the axis enum to these constants.
//
// The bench-test sketch firmware/t41-stepper-test/ also consumes this
// header (via -I in its platformio.ini) so bench and production
// firmware share a single source of truth for the carrier wiring.
//
// Topology mapping per docs/HW-T41-PINMAP.md §7:
//   L-Match : X = roller inductor (L), Y = vacuum cap (C), Z unused
//   T-Match : X = series C1,           Y = series C2,      Z = shunt L
//   Pi-Match: X = shunt C1,            Y = series L,       Z = shunt C2

#include <cstdint>

namespace hal::board::t41_v209 {

// ── Per-axis stepper outputs ──────────────────────────────────────────
// One screw-terminal channel per axis on the carrier. STEP / DIR drive
// the external stepper driver (TMC2209 / DM542 / TB6600) via opto-
// isolated inputs. EN is active-LOW on most external drivers — see
// EN_ACTIVE_LOW below.

struct axis_pins_t {
    uint8_t step;
    uint8_t dir;
    uint8_t en;
    uint8_t limit;  // mechanical home microswitch, opto-isolated
};

constexpr axis_pins_t AXIS_X  = { 2,  3,  10, 20 };
constexpr axis_pins_t AXIS_Y  = { 4,  5,  40, 21 };
constexpr axis_pins_t AXIS_Z  = { 6,  7,  39, 22 };

// Spare axes — wired on the carrier but unused by any current
// topology. Reserved for bandswitch / antenna-select / future
// expansion. Captured here so the pin numbers don't have to be
// re-discovered later.
constexpr axis_pins_t AXIS_M3 = { 8,  9,  38, 23 };
constexpr axis_pins_t AXIS_M4 = { 26, 27, 37, 28 };

constexpr uint8_t NUM_AXES_WIRED = 5;
constexpr uint8_t NUM_AXES_TUNER = 3;  // max under any supported topology

// ── Signal polarity ───────────────────────────────────────────────────
// External drivers (TMC2209 / DM542 / TB6600) use opto-isolated inputs;
// energising the opto pulls the input low. For the EN input that means
// LOW = driver enabled. Confirmed against TB6600 on the bench
// (PROPOSAL.md "Bench-test learnings"); the production driver-aware HAL
// should re-confirm per driver model before flipping a high-current
// axis. Limit switches are wired the same way: opto conducting =
// switch closed = limit asserted = LOW on the Teensy pin.
constexpr bool EN_ACTIVE_LOW    = true;
constexpr bool LIMIT_ACTIVE_LOW = true;

// Minimum STEP pulse width the external driver will reliably latch.
// TMC2209 in StealthChop tolerates AccelStepper's 1 µs default, but
// TB6600 needs ≥ 5 µs (PROPOSAL.md "Bench-test learnings"). 5 µs is
// safe for both and matches the bench-test setting.
constexpr uint8_t STEP_MIN_PULSE_US = 5;

// ── Relay-driver outputs ──────────────────────────────────────────────
// Carrier outputs intended for relay coils. Drive the external
// vacuum-relay coils via opto-isolated MOSFET stages; the HV bias side
// is independent. See docs/HW-T41-PINMAP.md §5.
//
// K1 / K2 are L-Match-only — the Hi-Z / Lo-Z selector relay pair is
// unused in the symmetric T / Pi topologies. K3 (bypass) is retained
// across all topologies per CLAUDE.md invariant 2.
constexpr uint8_t RELAY_K1_HIZ    = 12;  // SPINDLE EN
constexpr uint8_t RELAY_K2_LOZ    = 11;  // SPINDLE DIR
constexpr uint8_t RELAY_K3_BYPASS = 19;  // COOLANT FLOOD (latched at power-up)

// ── Control inputs (repurposed from GRBL function names) ──────────────
// Five opto-isolated inputs the carrier exposes as RESET / PROBE /
// FEED_HOLD / CYCLE_START / SAFETY_DOOR for CNC use. The tuner reuses
// them for its own operational signals — see docs/HW-T41-PINMAP.md §3.
constexpr uint8_t INPUT_OPERATOR_RESET = 14;  // RESET       — operator panic
constexpr uint8_t INPUT_RF_PRESENCE    = 15;  // PROBE       — external RF-detect (spare)
constexpr uint8_t INPUT_TX_PANIC       = 16;  // FEED_HOLD   — hardware TX-key lockout
constexpr uint8_t INPUT_ENGAGE         = 17;  // CYCLE_START — engage from bypass
constexpr uint8_t INPUT_INTERLOCK      = 29;  // SAFETY_DOOR — enclosure interlock

// ── Quadrature encoder inputs (X axis, mux'd with AUXINPUT1..3) ───────
// Per CLAUDE.md invariant 3 the encoder is the position truth. The
// carrier wires QEI A / B / SELECT to dedicated EMI-filtered Schmitt-
// triggered AUX inputs that double as the QEI pins on the Teensy 4.1
// silicon. See docs/HW-T41-PINMAP.md §4.
constexpr uint8_t ENC_X_A     = 30;  // QEI_A
constexpr uint8_t ENC_X_B     = 34;  // QEI_B
constexpr uint8_t ENC_X_INDEX = 35;  // QEI_SELECT / Z-pulse

}  // namespace hal::board::t41_v209
