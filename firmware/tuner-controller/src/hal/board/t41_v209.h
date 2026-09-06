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
// numbers directly. The real-driver HAL implementations
// (motor_teensy41.cpp, relay_teensy41.cpp, limits_teensy41.cpp) translate
// the axis index to these constants.
//
// The bench rig firmware/test/t41-stepper-test/ also consumes this
// header (via -I in its platformio.ini) so bench and production
// firmware share a single source of truth for the carrier wiring.
//
// Element → axis mapping is the operator's install-time choice
// (`set_topology`, persisted by app/config.cpp — CLAUDE.md "Topology vs
// firmware"). Defaults:
//   Balanced L : X = inductor pair (L), Y = capacitor (C),     Z spare
//   Balanced Pi: X = C1,                Y = inductor pair (L), Z = C2

#include <cstdint>

namespace hal::board::t41_v209 {

// ── Per-axis stepper outputs ──────────────────────────────────────────
// One screw-terminal channel per axis on the carrier. STEP / DIR / EN
// drive the external drive's opto-isolated inputs — the JMC iHSS60
// integrated closed-loop drive in the production build (CLAUDE.md
// "Hardware contract"), or any STEP/DIR driver on the bench. EN is
// active-LOW on all of them — see EN_ACTIVE_LOW below.

struct axis_pins_t {
    uint8_t step;
    uint8_t dir;
    uint8_t en;
    uint8_t limit;  // lead-screw home + max switches, NC in series, opto-isolated
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
// The drives' STEP / DIR / EN inputs are opto-isolated; energising the
// opto pulls the input low, so LOW = driver enabled on EN. Confirmed on
// the bench for TB6600 and iHSS60 (PROPOSAL.md "Bench-test learnings").
// Limit inputs are wired the same way: opto conducting = LOW = asserted,
// which with the NC-in-series home + max switches means "a switch has
// opened — at a limit, or a cable fault". Pulse timing (≥ 2.5 µs per
// level, DIR setup / hold) is owned by the FlexPWM driver in
// firmware/lib/flexpwm_stepper/, not by this pin map.
constexpr bool EN_ACTIVE_LOW    = true;
constexpr bool LIMIT_ACTIVE_LOW = true;

// ── Relay-driver outputs ──────────────────────────────────────────────
// Carrier outputs intended for relay coils. Drive the external
// vacuum-relay coils via opto-isolated MOSFET stages; the HV bias side
// is independent. See docs/HW-T41-PINMAP.md §5.
//
// K1 / K2 are Balanced-L-only — the Hi-Z / Lo-Z selector relay pair is
// absent in the Balanced Pi. Every switch is two-pole (both line legs)
// from one driver output. K3 (bypass) is retained across both
// topologies per CLAUDE.md invariant 2; hal/relay_teensy41.cpp maps
// bypass to the de-energised coil state.
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
// Reserved for an external QEI on a non-integrated motor (Phase-2
// fallback). With the iHSS60 the drive's encoder is internal and the
// FlexPWM step counter is the position source (CLAUDE.md invariant 3).
// The carrier wires QEI A / B / SELECT to dedicated EMI-filtered
// Schmitt-triggered AUX inputs that double as the QEI pins on the
// Teensy 4.1 silicon. See docs/HW-T41-PINMAP.md §4.
constexpr uint8_t ENC_X_A     = 30;  // QEI_A
constexpr uint8_t ENC_X_B     = 34;  // QEI_B
constexpr uint8_t ENC_X_INDEX = 35;  // QEI_SELECT / Z-pulse

}  // namespace hal::board::t41_v209
