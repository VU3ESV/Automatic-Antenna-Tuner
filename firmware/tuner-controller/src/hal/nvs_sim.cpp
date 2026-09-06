// Simulation backend for hal::nvs: a RAM array that survives
// app::motion::init() re-runs within one process, which is how the
// native tests simulate a power cycle. The Teensy 4.1 build uses the
// emulated EEPROM in nvs_teensy41.cpp.

#if !defined(TARGET_TEENSY41)

#include "hal/hal.h"

#include <cstring>

namespace hal::nvs {

namespace {
uint8_t store[kSize];
bool    initialised = false;
}

void init() {
    if (!initialised) {
        memset(store, 0xFF, sizeof(store));   // blank flash reads as 0xFF
        initialised = true;
    }
}

void read(size_t offset, void *buf, size_t n) {
    if (offset >= kSize) return;
    if (offset + n > kSize) n = kSize - offset;
    memcpy(buf, store + offset, n);
}

void write(size_t offset, const void *buf, size_t n) {
    if (offset >= kSize) return;
    if (offset + n > kSize) n = kSize - offset;
    memcpy(store + offset, buf, n);
}

} // namespace hal::nvs

#endif // !TARGET_TEENSY41
