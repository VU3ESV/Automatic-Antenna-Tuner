#pragma once

// Firmware update over the network — the policy half of OTA. The HTTP
// server streams an Intel-HEX file (PlatformIO's firmware.hex) into
// feed(); this module parses the records, stages the image through
// hal::firmware, verifies it and computes its CRC-32, and applies it only
// on an explicit second command that repeats the record count (the same
// "type the line count back" confirmation FlasherX uses on a serial
// console). The apply itself runs from tick() a few hundred ms later so
// the HTTP reply has left the socket before the MCU reboots.
//
// Gates on apply (the controller is about to reboot and stop servicing
// everything): bypass engaged (invariant 2 holds through the reboot), no
// axis moving, no RF. They are checked when the apply is requested AND
// again in tick() just before the copy, because the main loop keeps
// serving verbs in between; app::motion refuses motion / relay verbs
// with `updating` while apply_pending(). Settings are flushed to the card
// first; position anchors are already in EEPROM (invariant 3) and the
// staging buffer is kept below the EEPROM emulation region, so an update
// never touches them.
//
// Erasing the staging buffer (abort, a failed transfer, a new begin())
// runs with interrupts masked for the duration of each sector erase —
// tens of ms each, seconds in total — during which the step-counting ISR
// would miss pulses. Every entry point that can erase is therefore
// refused while an axis moves.
//
// Application-layer code: no platform headers, no JSON; testable on native
// against hal/firmware_sim.cpp.

#include <cstddef>
#include <cstdint>

#include "app/refusal.h"

namespace app::ota {

enum class State : uint8_t {
    Idle,          // nothing staged
    Receiving,     // begin() done, feed() in progress
    Staged,        // finish() verified the image — waiting for request_apply()
    ApplyPending,  // request_apply() accepted — tick() will reboot shortly
    Error,         // last transfer failed; code / msg say why. begin() clears it.
};
const char *state_name(State s);

struct Status {
    State       state     = State::Idle;
    bool        supported = false;
    const char *target    = "";        // id the image must contain (hal::firmware::target_id)
    uint32_t    lines     = 0;         // hex records consumed
    uint32_t    bytes     = 0;         // image bytes written
    uint32_t    min_addr  = 0;         // absolute flash range covered by data records: [min, max)
    uint32_t    max_addr  = 0;
    uint32_t    capacity  = 0;         // staging buffer size
    uint32_t    crc32     = 0;         // CRC-32 (IEEE 802.3) of the staged image; gaps count as 0xFF
    const char *code      = "";        // refusal code of the last error, or `apply_aborted` while Staged
    const char *msg       = "";
};

// Start receiving a hex file. Discards a staged image; refused with
// `busy` while an apply is pending, `moving` while any axis runs (the
// transfer blocks the loop and the discard erases flash), `unsupported` /
// `no_buffer` when the HAL cannot stage.
bool begin(Refusal &e);

// Feed raw hex text in any chunking (lines may split across calls).
// Records must be in ascending address order without overlap (objcopy
// output is); nothing but blank lines may follow the EOF record.
// false → State::Error with e.code one of bad_hex, bad_image, too_big,
// flash_write.
bool feed(const char *data, size_t len, Refusal &e);

// End of transfer: the EOF record must have been seen, the image must
// start at the flash base and carry the target id. Computes the CRC.
// false → State::Error (bad_hex, bad_image, wrong_target, flash_read).
bool finish(Refusal &e);

// Apply the staged image: `lines` must equal Status::lines. Gates:
// not_staged, bad_args, not_bypassed, moving, rf_lockout. On success the
// settings are flushed to the card and tick() reboots after a short
// delay — unless a gate fails again at that point, in which case the
// state drops back to Staged with code `apply_aborted` and the request
// must be repeated.
bool request_apply(uint32_t lines, Refusal &e);

// Drop whatever is staged (erases the flash buffer) → Idle. Refused with
// `busy` once an apply is pending and `moving` while any axis runs.
bool abort(Refusal &e);

// Drop a transfer in progress (State::Receiving) without gates — for the
// HTTP handler when the client disappears mid-body. Safe there because
// begin() excluded motion and the handler holds the main loop.
void cancel_transfer();

// True between an accepted request_apply() and the reboot. app::motion
// refuses motion and relay verbs while this holds.
bool apply_pending();

// Call once per main-loop iteration; performs the delayed apply.
void tick(uint32_t now_ms);

const Status &status();

// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). Start with
// 0xFFFFFFFF, feed chunks, invert the result: crc32 = ~update(...).
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

} // namespace app::ota
