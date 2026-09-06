// hal::nvs on the Teensy 4.1: the Teensyduino emulated EEPROM (4 KB,
// wear-levelled across a reserved flash sector). write() uses
// EEPROM.update() per byte so unchanged bytes are never rewritten —
// the app re-saves each axis position after every move and that must
// not cost flash endurance when the value is the same.
//
// STM32H7 port: back this with a flash page + the same update
// semantics, or an external FRAM; the app layout (app/config.cpp)
// stays within hal::nvs::kSize bytes.

#ifdef TARGET_TEENSY41

#include "hal/hal.h"

#include <Arduino.h>
#include <EEPROM.h>

namespace hal::nvs {

void init() {
    static_assert(kSize <= 4096, "Teensy 4.1 emulated EEPROM is 4 KB");
}

void read(size_t offset, void *buf, size_t n) {
    if (offset >= kSize) return;
    if (offset + n > kSize) n = kSize - offset;
    uint8_t *out = static_cast<uint8_t *>(buf);
    for (size_t i = 0; i < n; i++) out[i] = EEPROM.read(static_cast<int>(offset + i));
}

void write(size_t offset, const void *buf, size_t n) {
    if (offset >= kSize) return;
    if (offset + n > kSize) n = kSize - offset;
    const uint8_t *in = static_cast<const uint8_t *>(buf);
    for (size_t i = 0; i < n; i++) EEPROM.update(static_cast<int>(offset + i), in[i]);
}

} // namespace hal::nvs

#endif // TARGET_TEENSY41
