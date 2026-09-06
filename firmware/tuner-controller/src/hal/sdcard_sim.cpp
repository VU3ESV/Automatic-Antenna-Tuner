// Simulation backend for hal::sdcard: a handful of in-memory files
// (native tests, STM32 placeholder). Card presence can be toggled by
// the tests through sim_set_present(); sim_format() wipes the files.

#if !defined(TARGET_TEENSY41)

#include "hal/hal.h"

#include <cstring>

namespace hal::sdcard {

namespace {

struct File {
    bool   used = false;
    char   path[64];
    char   data[4096];
    size_t len = 0;
};

File files[4];
bool card_inserted = true;

File *find(const char *path) {
    for (auto &f : files) if (f.used && strcmp(f.path, path) == 0) return &f;
    return nullptr;
}

} // namespace

void sim_set_present(bool inserted) { card_inserted = inserted; }

void sim_format() {
    for (auto &f : files) f.used = false;
}

bool init()    { return card_inserted; }
bool present() { return card_inserted; }

int read_file(const char *path, char *buf, size_t max) {
    if (!present() || !buf || max == 0) return -1;
    const File *f = find(path);
    if (!f) return -1;
    size_t n = f->len < max - 1 ? f->len : max - 1;
    memcpy(buf, f->data, n);
    buf[n] = '\0';
    return static_cast<int>(n);
}

bool write_file(const char *path, const char *data, size_t len) {
    if (!present() || len >= sizeof(File::data)) return false;
    File *f = find(path);
    if (!f) {
        for (auto &c : files) if (!c.used) { f = &c; break; }
        if (!f) return false;
        f->used = true;
        strncpy(f->path, path, sizeof(f->path) - 1);
        f->path[sizeof(f->path) - 1] = '\0';
    }
    memcpy(f->data, data, len);
    f->len = len;
    return true;
}

} // namespace hal::sdcard

#endif // !TARGET_TEENSY41
