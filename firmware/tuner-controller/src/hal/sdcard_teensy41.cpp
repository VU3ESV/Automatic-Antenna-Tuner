// hal::sdcard on the Teensy 4.1: the built-in microSD socket through
// Teensyduino's SD library (SdFat underneath, exFAT / FAT32).
//
// write_file() is an atomic replace: the new content goes to
// "<path>.new", which is then renamed over the old file, so a power
// loss mid-write leaves either the previous config.json or the complete
// new one, never a truncated file. The settings layer keeps EEPROM as
// the synchronous authority for position anchors; the card is the
// human-readable settings copy (app/settings.h).

#ifdef TARGET_TEENSY41

#include "hal/hal.h"

#include <Arduino.h>
#include <SD.h>

#include <cstdio>
#include <cstring>

namespace hal::sdcard {

namespace {
bool mounted = false;
}

bool init() {
    mounted = SD.begin(BUILTIN_SDCARD);
    if (mounted && !SD.exists("/tuner")) SD.mkdir("/tuner");
    return mounted;
}

bool present() {
    if (!mounted) return false;
    return SD.mediaPresent();
}

int read_file(const char *path, char *buf, size_t max) {
    if (!mounted || !buf || max == 0) return -1;
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;
    const int n = f.read(buf, max - 1);
    f.close();
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

bool write_file(const char *path, const char *data, size_t len) {
    if (!mounted) return false;
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    SD.remove(tmp);
    File f = SD.open(tmp, FILE_WRITE_BEGIN);
    if (!f) return false;
    const size_t w = f.write(reinterpret_cast<const uint8_t *>(data), len);
    f.flush();
    f.close();
    if (w != len) { SD.remove(tmp); return false; }
    if (SD.exists(path) && !SD.remove(path)) return false;
    return SD.sdfs.rename(tmp, path);
}

} // namespace hal::sdcard

#endif // TARGET_TEENSY41
