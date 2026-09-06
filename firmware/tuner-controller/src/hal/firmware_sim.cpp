// Simulation backend for hal::firmware (native tests, STM32 placeholder):
// a RAM staging buffer with the same contract as the flash-backed Teensy
// implementation. apply() only records that it was called, so the tests
// can drive the whole stage → verify → apply path without rebooting.

#if !defined(TARGET_TEENSY41)

#include "hal/hal.h"

#include <cstring>

namespace hal::firmware {

namespace {

constexpr uint32_t kCapacity = 32 * 1024;
constexpr uint32_t kBase     = 0x60000000UL;   // same convention as the Teensy so one hex layout serves both
constexpr char     kTarget[] = "fw_sim";

uint8_t  buf[kCapacity];
bool     active       = false;
bool     applied      = false;
uint32_t applied_size = 0;

} // namespace

// Test hooks.
bool     sim_applied()      { return applied; }
uint32_t sim_applied_size() { return applied_size; }
void     sim_reset()        { active = false; applied = false; applied_size = 0; memset(buf, 0xFF, sizeof(buf)); }

bool        supported()  { return true; }
const char *target_id()  { return kTarget; }
uint32_t    image_base() { return kBase; }

bool begin(uint32_t &capacity) {
    memset(buf, 0xFF, sizeof(buf));   // erased flash reads 0xFF
    active   = true;
    capacity = kCapacity;
    return true;
}

bool write(uint32_t off, const void *data, size_t len) {
    if (!active || off > kCapacity || len > kCapacity - off) return false;
    memcpy(buf + off, data, len);
    return true;
}

bool read(uint32_t off, void *out, size_t len) {
    if (!active || off > kCapacity || len > kCapacity - off) return false;
    memcpy(out, buf + off, len);
    return true;
}

bool verify(uint32_t image_size) {
    if (!active || image_size == 0 || image_size > kCapacity) return false;
    const size_t n = strlen(kTarget);
    for (uint32_t i = 0; i + n <= image_size; i++) {
        if (memcmp(buf + i, kTarget, n) == 0) return true;
    }
    return false;
}

void discard() {
    active = false;
    memset(buf, 0xFF, sizeof(buf));
}

void apply(uint32_t image_size) {
    if (!active) return;
    applied      = true;
    applied_size = image_size;
    active       = false;
}

} // namespace hal::firmware

#endif // !TARGET_TEENSY41
