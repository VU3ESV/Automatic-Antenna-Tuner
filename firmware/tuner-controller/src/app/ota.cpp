#include "app/ota.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "app/settings.h"
#include "hal/hal.h"

namespace app::ota {

namespace {

constexpr size_t   kLineMax      = 600;   // a 255-byte record is 521 characters
constexpr uint32_t kApplyDelayMs = 400;   // let the HTTP reply and its FIN leave first
constexpr size_t   kCrcChunk     = 256;

Status   st;
char     line[kLineMax];
size_t   line_len      = 0;
uint32_t ext_base      = 0;     // extended linear / segment base for 16-bit record addresses
bool     eof           = false;
uint32_t image_size    = 0;     // bytes from image_base() to max_addr
uint32_t apply_seen_ms = 0;
char     msg_buf[112];

void refresh_static() {
    st.supported = hal::firmware::supported();
    st.target    = hal::firmware::target_id();
}

void reset_parser() {
    line_len   = 0;
    ext_base   = 0;
    eof        = false;
    image_size = 0;
}

bool any_axis_busy() {
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        if (hal::motor::busy(a)) return true;
    }
    return false;
}

// Transfer failure: the partial stage is erased (the handler holds the
// loop and begin() excluded motion, so the erase cannot starve a move).
bool fail(Refusal &e, const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, ap);
    va_end(ap);
    hal::firmware::discard();
    st.state = State::Error;
    st.code  = code;
    st.msg   = msg_buf;
    e.code   = code;
    e.msg    = msg_buf;
    return false;
}

bool refuse(Refusal &e, const char *code, const char *msg) {
    e.code = code;
    e.msg  = msg;
    return false;
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexbyte(const char *p, uint8_t &out) {
    const int hi = hexval(p[0]), lo = hexval(p[1]);
    if (hi < 0 || lo < 0) return false;
    out = static_cast<uint8_t>((hi << 4) | lo);
    return true;
}

// One complete record in line[0..line_len). Intel HEX: ":LLAAAATT<data>CC".
// tools/ota_upload.py::parse_hex applies the same rules so both sides
// count the same `lines` and reject the same files.
bool process_line(Refusal &e) {
    const unsigned long no = st.lines + 1;
    if (line[0] != ':') return fail(e, "bad_hex", "record %lu: missing ':'", no);
    if (line_len < 11 || ((line_len - 1) & 1)) return fail(e, "bad_hex", "record %lu: malformed", no);

    uint8_t n = 0, ahi = 0, alo = 0, type = 0, cksum = 0;
    if (!hexbyte(line + 1, n) || !hexbyte(line + 3, ahi) || !hexbyte(line + 5, alo) || !hexbyte(line + 7, type)) {
        return fail(e, "bad_hex", "record %lu: bad hex digit", no);
    }
    if (line_len != 11u + 2u * n) return fail(e, "bad_hex", "record %lu: length field does not match", no);

    uint8_t  data[255];
    unsigned sum = n + ahi + alo + type;
    for (uint8_t i = 0; i < n; i++) {
        if (!hexbyte(line + 9 + 2 * i, data[i])) return fail(e, "bad_hex", "record %lu: bad hex digit", no);
        sum += data[i];
    }
    if (!hexbyte(line + 9 + 2 * n, cksum)) return fail(e, "bad_hex", "record %lu: bad checksum digit", no);
    if ((sum + cksum) & 0xFFu) return fail(e, "bad_hex", "record %lu: checksum mismatch", no);

    st.lines++;
    if (eof) return fail(e, "bad_hex", "record %lu: record after the EOF record", no);
    switch (type) {
    case 0x00: {   // data
        const uint32_t addr = ext_base + (static_cast<uint32_t>(ahi) << 8 | alo);
        const uint32_t base = hal::firmware::image_base();
        if (addr < base) return fail(e, "bad_image", "record %lu: address %08lX is below the flash base", no, static_cast<unsigned long>(addr));
        // Ascending, non-overlapping only: NOR flash cannot be programmed
        // twice, and the byte count / CRC assume each byte is written once.
        if (st.bytes > 0 && addr < st.max_addr) {
            return fail(e, "bad_hex", "record %lu: address %08lX overlaps or precedes earlier data", no, static_cast<unsigned long>(addr));
        }
        const uint32_t off = addr - base;
        if (off > st.capacity || n > st.capacity - off) {
            return fail(e, "too_big", "image exceeds the %lu-byte staging buffer", static_cast<unsigned long>(st.capacity));
        }
        if (n == 0) break;
        if (!hal::firmware::write(off, data, n)) return fail(e, "flash_write", "flash write failed at %08lX", static_cast<unsigned long>(addr));
        if (addr < st.min_addr) st.min_addr = addr;
        if (addr + n > st.max_addr) st.max_addr = addr + n;
        st.bytes += n;
        break;
    }
    case 0x01: eof = true; break;                                                       // EOF
    case 0x02:                                                                          // extended segment address
    case 0x04:                                                                          // extended linear address
        if (n != 2) return fail(e, "bad_hex", "record %lu: extended address record must carry 2 bytes", no);
        ext_base = (static_cast<uint32_t>(data[0]) << 8 | data[1]) << (type == 0x02 ? 4 : 16);
        break;
    case 0x03:                                                                          // start segment address
    case 0x05:                                                                          // start linear address — informational
        if (n != 4) return fail(e, "bad_hex", "record %lu: start address record must carry 4 bytes", no);
        break;
    default:   return fail(e, "bad_hex", "record %lu: unknown record type %u", no, type);
    }
    return true;
}

} // namespace

const char *state_name(State s) {
    switch (s) {
    case State::Idle:         return "idle";
    case State::Receiving:    return "receiving";
    case State::Staged:       return "staged";
    case State::ApplyPending: return "applying";
    case State::Error:        return "error";
    }
    return "?";
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
    }
    return crc;
}

bool begin(Refusal &e) {
    if (st.state == State::ApplyPending) return refuse(e, "busy", "an update is being applied");
    if (!hal::firmware::supported())     return refuse(e, "unsupported", "no firmware-update backend on this target");
    // The transfer blocks the main loop for seconds and discarding an
    // earlier stage erases flash with interrupts masked: nothing may be in
    // motion.
    if (any_axis_busy()) return refuse(e, "moving", "stop every axis before uploading firmware");
    if (st.state == State::Receiving || st.state == State::Staged) hal::firmware::discard();
    st = Status{};
    refresh_static();
    reset_parser();
    st.min_addr = 0xFFFFFFFFUL;
    uint32_t cap = 0;
    if (!hal::firmware::begin(cap)) return fail(e, "no_buffer", "no free flash above the program for a staging buffer");
    st.capacity = cap;
    st.state    = State::Receiving;
    return true;
}

bool feed(const char *data, size_t len, Refusal &e) {
    if (st.state != State::Receiving) return refuse(e, "not_receiving", "call begin before feeding hex data");
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                const bool ok = process_line(e);
                line_len = 0;
                if (!ok) return false;
            }
            continue;
        }
        if (line_len + 1 >= kLineMax) return fail(e, "bad_hex", "record %lu: line too long", static_cast<unsigned long>(st.lines + 1));
        line[line_len++] = c;
    }
    return true;
}

bool finish(Refusal &e) {
    if (st.state != State::Receiving) return refuse(e, "not_receiving", "nothing is being received");
    if (line_len > 0) {                       // file without a trailing newline
        line[line_len] = '\0';
        const bool ok = process_line(e);
        line_len = 0;
        if (!ok) return false;
    }
    if (!eof)           return fail(e, "bad_hex", "transfer ended without an EOF record");
    if (st.bytes == 0)  return fail(e, "bad_hex", "empty image");
    const uint32_t base = hal::firmware::image_base();
    if (st.min_addr != base) {
        return fail(e, "bad_image", "image starts at %08lX, expected %08lX", static_cast<unsigned long>(st.min_addr), static_cast<unsigned long>(base));
    }
    image_size = st.max_addr - base;
    if (!hal::firmware::verify(image_size)) {
        return fail(e, "wrong_target", "image was not built for %s (target id missing)", hal::firmware::target_id());
    }
    uint32_t crc = 0xFFFFFFFFUL;
    uint8_t  chunk[kCrcChunk];
    for (uint32_t off = 0; off < image_size; off += kCrcChunk) {
        const size_t n = (image_size - off) < kCrcChunk ? (image_size - off) : kCrcChunk;
        if (!hal::firmware::read(off, chunk, n)) return fail(e, "flash_read", "read-back failed at offset %lu", static_cast<unsigned long>(off));
        crc = crc32_update(crc, chunk, n);
    }
    st.crc32 = ~crc;
    st.state = State::Staged;
    return true;
}

bool request_apply(uint32_t lines, Refusal &e) {
    if (st.state != State::Staged) return refuse(e, "not_staged", "no verified image is staged");
    if (lines != st.lines)         return refuse(e, "bad_args", "lines must equal the staged record count");
    if (!hal::relay::bypass())     return refuse(e, "not_bypassed", "engage bypass before applying firmware");
    if (any_axis_busy())           return refuse(e, "moving", "stop every axis before applying firmware");
    if (hal::safety::rf_present()) return refuse(e, "rf_lockout", "fwd_w exceeds tx_lockout_w");
    settings::flush();             // best effort — positions are already in EEPROM
    st.state      = State::ApplyPending;
    st.code       = "";
    st.msg        = "";
    apply_seen_ms = 0;
    return true;
}

void cancel_transfer() {
    if (st.state != State::Receiving) return;
    hal::firmware::discard();
    st = Status{};
    refresh_static();
    reset_parser();
}

bool abort(Refusal &e) {
    if (st.state == State::ApplyPending) return refuse(e, "busy", "an update is being applied - too late to discard it");
    if (any_axis_busy())                 return refuse(e, "moving", "stop every axis before discarding staged firmware");
    hal::firmware::discard();
    st = Status{};
    refresh_static();
    reset_parser();
    return true;
}

bool apply_pending() { return st.state == State::ApplyPending; }

void tick(uint32_t now_ms) {
    if (st.state != State::ApplyPending) return;
    if (apply_seen_ms == 0) { apply_seen_ms = now_ms ? now_ms : 1; return; }
    if (now_ms - apply_seen_ms < kApplyDelayMs) return;
    // The main loop kept serving the master link and the browser during
    // the delay. Re-check every gate the request passed; if anything
    // changed the image stays staged and the operator must ask again once
    // the tuner is inert. Never stop a moving axis from here.
    const char *why = nullptr;
    if (!hal::relay::bypass())          why = "bypass was disengaged during the apply delay";
    else if (any_axis_busy())           why = "an axis was moving when the apply came due";
    else if (hal::safety::rf_present()) why = "RF appeared during the apply delay";
    if (why) {
        st.state      = State::Staged;
        st.code       = "apply_aborted";
        st.msg        = why;
        apply_seen_ms = 0;
        return;
    }
    hal::firmware::apply(image_size);   // reboots on hardware
    st.state = State::Idle;             // sim only
    reset_parser();
}

const Status &status() {
    refresh_static();
    return st;
}

} // namespace app::ota
