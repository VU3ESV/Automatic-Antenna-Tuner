// hal::firmware on the Teensy 4.1: in-application flash programming over
// the vendored FlasherX flash layer (firmware/lib/flasherx, public domain).
//
// Layout (FlashTxx.c):
//   [ running program ][ staging buffer (erased) ][ FLASH_RESERVE: EEPROM emulation ]
//   ^0x60000000                                  ^0x607C0000                 ^0x60800000
//
// begin()   erases anything left between the running program's end
//           (linker symbol _flashimagelen) and FLASH_RESERVE — a stage
//           abandoned by a reboot mid-transfer would otherwise look like
//           program to firmware_buffer_init() and lose its sectors for
//           good — then firmware_buffer_init(): the first erased sector
//           above the program up to FLASH_RESERVE. The reserve is patched
//           to 256 KB because Teensyduino's EEPROM emulation (hal::nvs —
//           settings and position anchors) lives in the top 64 sectors.
// write()   stages bytes through a 256-byte page buffer in RAM and programs
//           each page with the core's eepromemu_flash_write() — one
//           program operation per page instead of FlasherX's per-word
//           writes. Every page is read back and compared.
// verify()  flushes the page and checks the image carries FLASH_ID
//           ("fw_aat_teensy41"), i.e. it is this project's firmware built
//           with this file linked in — anything else is refused.
// apply()   flash_move() from RAM-resident code with interrupts masked
//           throughout (local patch): erase + copy sector by sector, erase
//           the buffer, reboot. Does not return. Erase-dominated: ~134
//           sector erases for a 270 KB image, several seconds typical,
//           tens of seconds worst case. Power loss inside it leaves no
//           program — the USB bootloader (separate MKL02 chip) is the
//           recovery path.
// discard() erases the buffer so a later begin() finds clean flash. Each
//           sector erase masks interrupts for its duration, so callers
//           (app::ota) never allow it while an axis moves.

#ifdef TARGET_TEENSY41

#include "hal/hal.h"

#include <Arduino.h>

#include <cstring>

extern "C" {
#include "FlashTxx.h"
}

namespace hal::firmware {

namespace {

// The buffer (and flash_move's post-copy erase) must stop below the
// Teensyduino EEPROM emulation region — cores/teensy4/eeprom.c places it
// at 0x607C0000 on the Teensy 4.1.
static_assert(FLASH_BASE_ADDR + FLASH_SIZE - FLASH_RESERVE <= 0x607C0000UL,
              "FlasherX staging buffer would overlap the Teensy 4.1 EEPROM emulation — check FLASH_RESERVE");

constexpr uint32_t kPage = 256;   // W25Q64 program page

extern "C" unsigned long _flashimagelen;   // linker: byte length of the running image in flash

// Erase whatever sits between the program and the reserve so the buffer
// always starts right above the running image (see begin() above).
void reclaim_stale() {
    uint32_t end = FLASH_BASE_ADDR + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&_flashimagelen));
    end = (end + FLASH_SECTOR_SIZE - 1) & ~static_cast<uint32_t>(FLASH_SECTOR_SIZE - 1);
    const uint32_t top = FLASH_BASE_ADDR + FLASH_SIZE - FLASH_RESERVE;
    if (end < top) flash_erase_block(end, top - end);   // erases only non-erased sectors
}

uint32_t buf_addr = 0;            // absolute flash address of the staging buffer
uint32_t buf_size = 0;
bool     active   = false;

uint8_t  page[kPage] __attribute__((aligned(4)));   // RAM — flash_write needs a RAM source
uint32_t page_off = 0;            // image offset of page[0]
uint32_t page_len = 0;

bool flush_page() {
    if (page_len == 0) return true;
    void *dst = reinterpret_cast<void *>(buf_addr + page_off);
    eepromemu_flash_write(dst, page, page_len);
    const bool ok = memcmp(dst, page, page_len) == 0;   // read back through the (invalidated) cache
    page_len = 0;
    return ok;
}

} // namespace

bool        supported()  { return true; }
const char *target_id()  { return FLASH_ID; }
uint32_t    image_base() { return FLASH_BASE_ADDR; }

bool begin(uint32_t &capacity) {
    if (active) discard();
    page_len = 0;
    reclaim_stale();
    const int type = firmware_buffer_init(&buf_addr, &buf_size);
    if (type == NO_BUFFER_TYPE || buf_size < FLASH_SECTOR_SIZE) {
        buf_size = 0;
        capacity = 0;
        return false;
    }
    active   = true;
    capacity = buf_size;
    return true;
}

bool write(uint32_t off, const void *data, size_t len) {
    if (!active || off > buf_size || len > buf_size - off) return false;
    const uint8_t *p = static_cast<const uint8_t *>(data);
    while (len > 0) {
        // A gap or a backwards jump closes the pending page.
        if (page_len > 0 && off != page_off + page_len && !flush_page()) return false;
        if (page_len == 0) page_off = off;
        const uint32_t room = kPage - ((buf_addr + off) % kPage);   // bytes left in this flash page
        const size_t   n    = len < room ? len : room;
        memcpy(page + page_len, p, n);
        page_len += n;
        off      += n;
        p        += n;
        len      -= n;
        if ((buf_addr + off) % kPage == 0 && !flush_page()) return false;   // page complete
    }
    return true;
}

bool read(uint32_t off, void *out, size_t len) {
    if (!active || off > buf_size || len > buf_size - off) return false;
    if (!flush_page()) return false;
    memcpy(out, reinterpret_cast<const void *>(buf_addr + off), len);
    return true;
}

bool verify(uint32_t image_size) {
    if (!active || image_size == 0 || image_size > buf_size) return false;
    if (!flush_page()) return false;
    return check_flash_id(buf_addr, image_size) != 0;
}

void discard() {
    if (!active) return;
    page_len = 0;
    firmware_buffer_free(buf_addr, buf_size);   // erases only the sectors that were written
    active   = false;
    buf_size = 0;
}

void apply(uint32_t image_size) {
    if (!active || image_size == 0 || image_size > buf_size) return;
    // flash_move() copies whole words; the padding bytes are erased 0xFF
    // in the buffer and program as no-ops.
    const uint32_t size = (image_size + 3) & ~3u;
    if (size > buf_size) return;
    flush_page();
    Serial.flush();
    flash_move(FLASH_BASE_ADDR, buf_addr, size);   // never returns: reboots into the new image
}

} // namespace hal::firmware

#endif // TARGET_TEENSY41
