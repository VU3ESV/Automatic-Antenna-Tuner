#pragma once

// Settings persistence policy: emulated EEPROM (hal::nvs) + microSD
// card (hal::sdcard) working together.
//
//   EEPROM  — always present, written synchronously. Authority for the
//             per-axis position anchors and the "move in flight" dirty
//             marker (invariant 3), which must land the instant a move
//             starts or ends. Also a mirror of every setting, so a boot
//             without a card still has the full configuration.
//   SD card — human-readable `/tuner/config.json`: topology + element→
//             motor map, per-axis element kind, rated travel, speed,
//             accel, home flag, plus the last saved positions for
//             information. Wins over EEPROM for *settings* at boot when
//             present and valid (so a card can carry a configuration
//             between boards or be edited on a PC), and is rewritten
//             a short debounce after any settings change. Positions on
//             the card are informational only — EEPROM's are used.
//
// Application-layer code; JSON via ArduinoJson (portable). The SD and
// EEPROM drivers are behind the HAL so the STM32 port only swaps those.

#include <cstddef>
#include <cstdint>

#include "app/config.h"

namespace app::settings {

constexpr const char *kConfigPath = "/tuner/config.json";

struct Status {
    bool           sd_present = false;   // card mounted
    bool           sd_ok      = false;   // config.json read or written successfully
    SettingsSource source     = SettingsSource::Defaults;
    uint32_t       sd_saves   = 0;       // files written this boot
};

// Load per the policy above into `cfg`. Mounts the card, initialises
// the EEPROM store. Creates / repairs the card copy when needed.
void init(Persisted &cfg);

// Persist one change: EEPROM immediately, card copy scheduled.
void save_topology(const Topology &t);
void save_axis(uint8_t axis, const AxisConfig &c);
void save_position(uint8_t axis, int32_t pos, bool dirty);

// Flush a scheduled card write once the debounce has elapsed; also
// tracks card insertion / removal. Call once per main-loop iteration.
void tick(uint32_t now_ms);

// Write the card copy now (if a card is present). Returns success.
bool flush();

const Status &status();

// The live persisted record (what the card copy should contain): the
// current settings, generation, and the last saved positions.
const Persisted &current();

// Raw text of the card's config.json (for inspection). Bytes read, or
// -1 when there is no card / no file.
int read_card_copy(char *buf, size_t max);

// The JSON codec, public for the HTTP export and the tests.
// to_json: bytes written (pretty-printed), or -1 if `max` is too small.
int  to_json(const Persisted &p, char *out, size_t max);
// from_json: false when the text is not a valid config (topology fails
// validate_topology(), bad kinds / ranges are individually defaulted).
bool from_json(const char *text, size_t len, Persisted &out);

} // namespace app::settings
