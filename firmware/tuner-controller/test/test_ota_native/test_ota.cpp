// Host-side unit tests for app::ota against hal/firmware_sim.cpp: Intel-HEX
// parsing in arbitrary chunking, the staging checks (checksum, record
// shapes, ordering, buffer size, image base, target id), the CRC, the
// apply / abort gates, the re-check of the gates when the delayed apply
// comes due, and app::motion's refusal of verbs while an apply is pending.

#include <unity.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/motion.h"
#include "app/ota.h"
#include "app/state.h"
#include "hal/hal.h"

// Sim-only hooks (hal/firmware_sim.cpp, hal/sdcard_sim.cpp).
namespace hal::firmware {
bool     sim_applied();
uint32_t sim_applied_size();
void     sim_reset();
}
namespace hal::sdcard { void sim_format(); }

namespace {

app::Refusal err;

// One Intel-HEX record with a correct checksum.
std::string rec(uint8_t type, uint16_t addr, const std::vector<uint8_t> &d) {
    char buf[16];
    std::string s = ":";
    unsigned sum = d.size() + (addr >> 8) + (addr & 0xFF) + type;
    snprintf(buf, sizeof(buf), "%02X%04X%02X", static_cast<unsigned>(d.size()), addr, type);
    s += buf;
    for (uint8_t b : d) { snprintf(buf, sizeof(buf), "%02X", b); s += buf; sum += b; }
    snprintf(buf, sizeof(buf), "%02X\n", static_cast<unsigned>((0x100 - (sum & 0xFF)) & 0xFF));
    s += buf;
    return s;
}

// A 64-byte image at 0x60000000 containing the sim target id, as 4 data
// records, framed by an extended-linear-address record and EOF → 6 lines.
std::vector<uint8_t> sample_image() {
    std::vector<uint8_t> img(64, 0x11);
    const char *id = "fw_sim";
    memcpy(&img[20], id, strlen(id));
    for (size_t i = 32; i < 64; i++) img[i] = static_cast<uint8_t>(i);
    return img;
}

std::string sample_hex(const std::vector<uint8_t> &img, bool with_eof = true) {
    std::string h = rec(0x04, 0, {0x60, 0x00});
    for (size_t off = 0; off < img.size(); off += 16) {
        h += rec(0x00, static_cast<uint16_t>(off), std::vector<uint8_t>(img.begin() + off, img.begin() + off + 16));
    }
    if (with_eof) h += rec(0x01, 0, {});
    return h;
}

uint32_t crc_of(const std::vector<uint8_t> &img) {
    return ~app::ota::crc32_update(0xFFFFFFFFUL, img.data(), img.size());
}

bool stage(const std::string &hex, size_t chunk) {
    if (!app::ota::begin(err)) return false;
    for (size_t i = 0; i < hex.size(); i += chunk) {
        const size_t n = hex.size() - i < chunk ? hex.size() - i : chunk;
        if (!app::ota::feed(hex.data() + i, n, err)) return false;
    }
    return app::ota::finish(err);
}

} // namespace

void setUp() {
    err = {nullptr, nullptr};
    hal::relay::init();
    hal::motor::init();
    hal::safety::init();
    // A test may end with an apply pending; let it run so abort() is not refused.
    if (app::ota::apply_pending()) { app::ota::tick(1); app::ota::tick(1000); }
    hal::firmware::sim_reset();
    TEST_ASSERT_TRUE(app::ota::abort(err));
}
void tearDown() {}

void test_crc32_reference_vector() {
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926UL, ~app::ota::crc32_update(0xFFFFFFFFUL, "123456789", 9));
    // Chunked == one-shot.
    uint32_t c = app::ota::crc32_update(0xFFFFFFFFUL, "1234", 4);
    c = app::ota::crc32_update(c, "56789", 5);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926UL, ~c);
}

void test_stage_whole_file() {
    const auto img = sample_image();
    TEST_ASSERT_TRUE(stage(sample_hex(img), 100000));
    const app::ota::Status &s = app::ota::status();
    TEST_ASSERT_TRUE(s.state == app::ota::State::Staged);
    TEST_ASSERT_EQUAL_UINT32(6, s.lines);
    TEST_ASSERT_EQUAL_UINT32(64, s.bytes);
    TEST_ASSERT_EQUAL_HEX32(0x60000000UL, s.min_addr);
    TEST_ASSERT_EQUAL_HEX32(0x60000040UL, s.max_addr);
    TEST_ASSERT_EQUAL_HEX32(crc_of(img), s.crc32);
    TEST_ASSERT_EQUAL_STRING("fw_sim", s.target);
    TEST_ASSERT_TRUE(s.supported);
    TEST_ASSERT_FALSE(hal::firmware::sim_applied());
}

void test_stage_in_small_chunks_and_crlf() {
    auto img = sample_image();
    std::string hex = sample_hex(img);
    // CRLF line endings and a missing trailing newline must both work.
    std::string crlf;
    for (char c : hex) { if (c == '\n') crlf += "\r\n"; else crlf += c; }
    crlf.erase(crlf.size() - 2);
    TEST_ASSERT_TRUE(stage(crlf, 7));
    TEST_ASSERT_EQUAL_UINT32(6, app::ota::status().lines);
    TEST_ASSERT_EQUAL_HEX32(crc_of(img), app::ota::status().crc32);
}

void test_bad_checksum_rejected_then_recoverable() {
    std::string hex = sample_hex(sample_image());
    hex[20] = (hex[20] == '0') ? '1' : '0';   // corrupt a data byte in the first data record
    TEST_ASSERT_FALSE(stage(hex, 100000));
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
    TEST_ASSERT_TRUE(app::ota::status().state == app::ota::State::Error);
    TEST_ASSERT_NOT_NULL(strstr(app::ota::status().msg, "checksum"));
    // A fresh begin() clears the error and a good file stages.
    TEST_ASSERT_TRUE(stage(sample_hex(sample_image()), 100000));
    TEST_ASSERT_TRUE(app::ota::status().state == app::ota::State::Staged);
}

void test_image_too_big_for_buffer() {
    // A record at offset 0x8000 (32 KB) is past the sim's 32 KB buffer.
    std::string hex = rec(0x04, 0, {0x60, 0x00}) + rec(0x00, 0x0000, std::vector<uint8_t>(16, 1))
                    + rec(0x00, 0x7FF8, std::vector<uint8_t>(16, 2)) + rec(0x01, 0, {});
    TEST_ASSERT_FALSE(stage(hex, 100000));
    TEST_ASSERT_EQUAL_STRING("too_big", err.code);
}

void test_missing_target_id_and_wrong_base() {
    std::vector<uint8_t> img(64, 0x22);   // no "fw_sim" anywhere
    TEST_ASSERT_FALSE(stage(sample_hex(img), 100000));
    TEST_ASSERT_EQUAL_STRING("wrong_target", err.code);
    // Image that does not start at the flash base.
    std::string hex = rec(0x04, 0, {0x60, 0x00}) + rec(0x00, 0x0100, sample_image()) + rec(0x01, 0, {});
    TEST_ASSERT_FALSE(stage(hex, 100000));
    TEST_ASSERT_EQUAL_STRING("bad_image", err.code);
    // Record below the flash base (no extended address record at all).
    hex = rec(0x00, 0x0000, std::vector<uint8_t>(16, 1)) + rec(0x01, 0, {});
    TEST_ASSERT_FALSE(stage(hex, 100000));
    TEST_ASSERT_EQUAL_STRING("bad_image", err.code);
}

void test_eof_rules() {
    TEST_ASSERT_FALSE(stage(sample_hex(sample_image(), /*with_eof=*/false), 100000));
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
    std::string hex = sample_hex(sample_image()) + rec(0x00, 0x0040, std::vector<uint8_t>(16, 3));
    TEST_ASSERT_FALSE(stage(hex, 100000));
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "after the EOF"));
}

void test_begin_refused_while_moving() {
    hal::motor::move_to(0, 100);   // sim: busy until ticked
    TEST_ASSERT_FALSE(app::ota::begin(err));
    TEST_ASSERT_EQUAL_STRING("moving", err.code);
    hal::motor::stop(0);
    TEST_ASSERT_TRUE(app::ota::begin(err));
}

void test_feed_requires_begin() {
    TEST_ASSERT_FALSE(app::ota::feed("x", 1, err));
    TEST_ASSERT_EQUAL_STRING("not_receiving", err.code);
    TEST_ASSERT_FALSE(app::ota::finish(err));
    TEST_ASSERT_FALSE(app::ota::request_apply(6, err));
    TEST_ASSERT_EQUAL_STRING("not_staged", err.code);
}

void test_apply_gates_then_delayed_apply() {
    TEST_ASSERT_TRUE(stage(sample_hex(sample_image()), 100000));
    // Wrong record count.
    TEST_ASSERT_FALSE(app::ota::request_apply(5, err));
    TEST_ASSERT_EQUAL_STRING("bad_args", err.code);
    // Network engaged.
    hal::relay::set_bypass(false);
    TEST_ASSERT_FALSE(app::ota::request_apply(6, err));
    TEST_ASSERT_EQUAL_STRING("not_bypassed", err.code);
    hal::relay::set_bypass(true);
    // An axis in motion.
    hal::motor::move_to(1, 500);
    TEST_ASSERT_FALSE(app::ota::request_apply(6, err));
    TEST_ASSERT_EQUAL_STRING("moving", err.code);
    hal::motor::stop(1);
    // RF present.
    hal::safety::inject_fwd_w(50.0f);
    TEST_ASSERT_FALSE(app::ota::request_apply(6, err));
    TEST_ASSERT_EQUAL_STRING("rf_lockout", err.code);
    hal::safety::inject_fwd_w(0.0f);
    // All clear: pending, then applied after the delay.
    TEST_ASSERT_TRUE(app::ota::request_apply(6, err));
    TEST_ASSERT_TRUE(app::ota::status().state == app::ota::State::ApplyPending);
    TEST_ASSERT_FALSE(app::ota::begin(err));            // no new transfer while applying
    TEST_ASSERT_EQUAL_STRING("busy", err.code);
    app::ota::tick(1000);
    app::ota::tick(1100);
    TEST_ASSERT_FALSE(hal::firmware::sim_applied());    // still inside the delay
    app::ota::tick(1500);
    TEST_ASSERT_TRUE(hal::firmware::sim_applied());
    TEST_ASSERT_EQUAL_UINT32(64, hal::firmware::sim_applied_size());
    TEST_ASSERT_TRUE(app::ota::status().state == app::ota::State::Idle);
}

void test_abort_and_restage() {
    TEST_ASSERT_TRUE(stage(sample_hex(sample_image()), 100000));
    TEST_ASSERT_TRUE(app::ota::abort(err));
    TEST_ASSERT_TRUE(app::ota::status().state == app::ota::State::Idle);
    TEST_ASSERT_EQUAL_UINT32(0, app::ota::status().lines);
    TEST_ASSERT_FALSE(app::ota::request_apply(6, err));
    // begin() while staged discards and starts over.
    TEST_ASSERT_TRUE(stage(sample_hex(sample_image()), 100000));
    TEST_ASSERT_TRUE(app::ota::begin(err));
    TEST_ASSERT_TRUE(app::ota::status().state == app::ota::State::Receiving);
    TEST_ASSERT_EQUAL_UINT32(0, app::ota::status().bytes);
}


// ── Record shapes the reviewers flagged as unchecked ────────────────────

void test_extended_and_start_address_record_lengths() {
    // Type 04 with 0 or 1 data bytes would read uninitialised bytes.
    TEST_ASSERT_FALSE(stage(rec(0x04, 0, {}) + sample_hex(sample_image()), 100000));
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "2 bytes"));
    TEST_ASSERT_FALSE(stage(rec(0x04, 0, {0x60}) + sample_hex(sample_image()), 100000));
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
    // Start-address records carry 4 bytes; a 2-byte one is malformed.
    TEST_ASSERT_FALSE(stage(rec(0x05, 0, {0x60, 0x00}) + sample_hex(sample_image()), 100000));
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "4 bytes"));
    // A well-formed one is informational and counts as a record.
    TEST_ASSERT_TRUE(stage(rec(0x05, 0, {0x60, 0x00, 0x00, 0x00}) + sample_hex(sample_image()), 100000));
    TEST_ASSERT_EQUAL_UINT32(7, app::ota::status().lines);
}

void test_segment_address_record_shift() {
    // Type 02 shifts by 4: 0x1000 → base 0x00010000, below the flash base.
    const std::string hex = rec(0x02, 0, {0x10, 0x00}) + rec(0x00, 0x0000, std::vector<uint8_t>(16, 1)) + rec(0x01, 0, {});
    TEST_ASSERT_FALSE(stage(hex, 100000));
    TEST_ASSERT_EQUAL_STRING("bad_image", err.code);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "00010000"));
}

void test_unknown_record_type() {
    TEST_ASSERT_FALSE(stage(rec(0x04, 0, {0x60, 0x00}) + rec(0x09, 0, {}) + sample_hex(sample_image()), 100000));
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "unknown record type"));
}

void test_lowercase_hex_digits() {
    const auto img = sample_image();
    std::string hex = sample_hex(img);
    for (char &c : hex) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    TEST_ASSERT_TRUE(stage(hex, 100000));
    TEST_ASSERT_EQUAL_HEX32(crc_of(img), app::ota::status().crc32);
}

void test_longest_record_and_overlong_line() {
    // A 255-byte record (521 characters) is the longest legal line.
    std::vector<uint8_t> img(255, 0x33);
    memcpy(&img[100], "fw_sim", 6);
    TEST_ASSERT_TRUE(stage(rec(0x04, 0, {0x60, 0x00}) + rec(0x00, 0, img) + rec(0x01, 0, {}), 100000));
    TEST_ASSERT_EQUAL_UINT32(255, app::ota::status().bytes);
    TEST_ASSERT_EQUAL_HEX32(crc_of(img), app::ota::status().crc32);
    // 600 characters with no newline are refused before the line completes.
    TEST_ASSERT_FALSE(stage(rec(0x04, 0, {0x60, 0x00}) + std::string(600, 'A'), 3));
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "too long"));
}

void test_overlapping_or_descending_records_rejected() {
    const auto img = sample_image();
    const std::vector<uint8_t> r0(img.begin(), img.begin() + 16), r1(img.begin() + 16, img.begin() + 32);
    // Same record twice: NOR flash cannot be programmed twice.
    std::string hex = rec(0x04, 0, {0x60, 0x00}) + rec(0x00, 0x0000, r0) + rec(0x00, 0x0010, r1) + rec(0x00, 0x0010, r1) + rec(0x01, 0, {});
    TEST_ASSERT_FALSE(stage(hex, 100000));
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "overlaps"));
    // Descending addresses (objcopy never emits them).
    hex = rec(0x04, 0, {0x60, 0x00}) + rec(0x00, 0x0010, r1) + rec(0x00, 0x0000, r0) + rec(0x01, 0, {});
    TEST_ASSERT_FALSE(stage(hex, 100000));
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
    // A gap is fine: bytes counts written bytes, the CRC covers the 0xFF gap.
    hex = rec(0x04, 0, {0x60, 0x00}) + rec(0x00, 0x0000, r0) + rec(0x00, 0x0020, std::vector<uint8_t>(img.begin() + 32, img.begin() + 48)) + rec(0x01, 0, {});
    std::vector<uint8_t> want(img.begin(), img.begin() + 48);
    for (size_t i = 16; i < 32; i++) want[i] = 0xFF;
    // Put the target id inside the first record so verify() passes.
    memcpy(&want[2], "fw_sim", 6);
    std::vector<uint8_t> r0id(want.begin(), want.begin() + 16);
    hex = rec(0x04, 0, {0x60, 0x00}) + rec(0x00, 0x0000, r0id) + rec(0x00, 0x0020, std::vector<uint8_t>(want.begin() + 32, want.begin() + 48)) + rec(0x01, 0, {});
    TEST_ASSERT_TRUE(stage(hex, 100000));
    TEST_ASSERT_EQUAL_UINT32(32, app::ota::status().bytes);
    TEST_ASSERT_EQUAL_HEX32(0x60000030UL, app::ota::status().max_addr);
    TEST_ASSERT_EQUAL_HEX32(crc_of(want), app::ota::status().crc32);
}

void test_only_blank_lines_may_follow_eof() {
    const std::string hex = sample_hex(sample_image());
    TEST_ASSERT_TRUE(stage(hex + "\n\r\n\n", 100000));
    TEST_ASSERT_EQUAL_UINT32(6, app::ota::status().lines);
    TEST_ASSERT_FALSE(stage(hex + rec(0x01, 0, {}), 100000));            // a second EOF
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "after the EOF"));
    TEST_ASSERT_FALSE(stage(hex + rec(0x04, 0, {0x00, 0x00}), 100000));  // an address record
    TEST_ASSERT_EQUAL_STRING("bad_hex", err.code);
}

// ── Gates around the erase and the reboot ───────────────────────────────

void test_abort_refused_while_moving_or_applying() {
    TEST_ASSERT_TRUE(stage(sample_hex(sample_image()), 100000));
    hal::motor::move_to(0, 100);   // sim: busy until ticked
    TEST_ASSERT_FALSE(app::ota::abort(err));
    TEST_ASSERT_EQUAL_STRING("moving", err.code);
    TEST_ASSERT_TRUE(app::ota::status().state == app::ota::State::Staged);
    TEST_ASSERT_EQUAL_UINT32(6, app::ota::status().lines);
    // begin() is refused the same way and also leaves the stage intact.
    TEST_ASSERT_FALSE(app::ota::begin(err));
    TEST_ASSERT_EQUAL_STRING("moving", err.code);
    TEST_ASSERT_TRUE(app::ota::status().state == app::ota::State::Staged);
    hal::motor::stop(0);
    // Once the apply is pending it is too late to discard.
    TEST_ASSERT_TRUE(app::ota::request_apply(6, err));
    TEST_ASSERT_TRUE(app::ota::apply_pending());
    TEST_ASSERT_FALSE(app::ota::abort(err));
    TEST_ASSERT_EQUAL_STRING("busy", err.code);
    app::ota::tick(1);
    app::ota::tick(1000);
    TEST_ASSERT_TRUE(hal::firmware::sim_applied());
    TEST_ASSERT_FALSE(app::ota::apply_pending());
    TEST_ASSERT_TRUE(app::ota::abort(err));   // idle: harmless
}

void test_apply_gates_rechecked_when_due() {
    TEST_ASSERT_TRUE(stage(sample_hex(sample_image()), 100000));
    // An axis starts moving inside the delay (through the HAL — app::motion
    // would refuse it, see the next test).
    TEST_ASSERT_TRUE(app::ota::request_apply(6, err));
    hal::motor::move_to(0, 100);
    app::ota::tick(1);
    app::ota::tick(1000);
    TEST_ASSERT_FALSE(hal::firmware::sim_applied());
    TEST_ASSERT_TRUE(app::ota::status().state == app::ota::State::Staged);
    TEST_ASSERT_EQUAL_STRING("apply_aborted", app::ota::status().code);
    TEST_ASSERT_TRUE(hal::motor::busy(0));   // ota never stops a moving axis
    hal::motor::stop(0);
    // Bypass dropped inside the delay.
    TEST_ASSERT_TRUE(app::ota::request_apply(6, err));
    TEST_ASSERT_EQUAL_STRING("", app::ota::status().code);
    hal::relay::set_bypass(false);
    app::ota::tick(2000);
    app::ota::tick(3000);
    TEST_ASSERT_FALSE(hal::firmware::sim_applied());
    TEST_ASSERT_TRUE(app::ota::status().state == app::ota::State::Staged);
    TEST_ASSERT_NOT_NULL(strstr(app::ota::status().msg, "bypass"));
    hal::relay::set_bypass(true);
    // RF inside the delay.
    TEST_ASSERT_TRUE(app::ota::request_apply(6, err));
    hal::safety::inject_fwd_w(50.0f);
    app::ota::tick(4000);
    app::ota::tick(5000);
    TEST_ASSERT_FALSE(hal::firmware::sim_applied());
    TEST_ASSERT_NOT_NULL(strstr(app::ota::status().msg, "RF"));
    hal::safety::inject_fwd_w(0.0f);
    // The image is still staged: a clean retry applies it.
    TEST_ASSERT_TRUE(app::ota::request_apply(6, err));
    app::ota::tick(6000);
    app::ota::tick(7000);
    TEST_ASSERT_TRUE(hal::firmware::sim_applied());
    TEST_ASSERT_EQUAL_UINT32(64, hal::firmware::sim_applied_size());
}

void test_motion_verbs_refused_while_apply_pending() {
    // A fresh controller through app::motion (same scaffold as test_motion).
    hal::nvs::init();
    hal::sdcard::sim_format();
    app::Persisted p;
    p.topology = app::Topology::default_balanced_l();
    app::nvs_format(p);
    app::motion::init();
    app::Snapshot snap;
    app::motion::tick(0, snap);
    TEST_ASSERT_TRUE(snap.bypass);
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 10, true, err)));
    app::motion::stop_all();
    for (uint32_t t = 1; t < 50; t++) app::motion::tick(t, snap);
    TEST_ASSERT_FALSE(snap.moving);

    TEST_ASSERT_TRUE(stage(sample_hex(sample_image()), 100000));
    TEST_ASSERT_TRUE(app::ota::request_apply(6, err));
    TEST_ASSERT_TRUE(app::motion::move_axis(0, 10, true, err) == app::motion::MoveResult::Refused);
    TEST_ASSERT_EQUAL_STRING("updating", err.code);
    TEST_ASSERT_FALSE(app::motion::set_bypass(false, err));
    TEST_ASSERT_EQUAL_STRING("updating", err.code);
    TEST_ASSERT_TRUE(hal::relay::bypass());
    TEST_ASSERT_TRUE(app::motion::set_bypass(true, err));   // engaging is always allowed
    TEST_ASSERT_FALSE(app::motion::set_side(app::Side::LoZ, err));
    TEST_ASSERT_EQUAL_STRING("updating", err.code);
    TEST_ASSERT_FALSE(app::motion::home(err));
    TEST_ASSERT_EQUAL_STRING("updating", err.code);
    app::ota::tick(100);
    app::ota::tick(600);
    TEST_ASSERT_TRUE(hal::firmware::sim_applied());
    // Back to normal once the (simulated) reboot has happened.
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 10, true, err)));
    app::motion::stop_all();
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_reference_vector);
    RUN_TEST(test_stage_whole_file);
    RUN_TEST(test_stage_in_small_chunks_and_crlf);
    RUN_TEST(test_bad_checksum_rejected_then_recoverable);
    RUN_TEST(test_image_too_big_for_buffer);
    RUN_TEST(test_missing_target_id_and_wrong_base);
    RUN_TEST(test_eof_rules);
    RUN_TEST(test_begin_refused_while_moving);
    RUN_TEST(test_feed_requires_begin);
    RUN_TEST(test_apply_gates_then_delayed_apply);
    RUN_TEST(test_abort_and_restage);
    RUN_TEST(test_extended_and_start_address_record_lengths);
    RUN_TEST(test_segment_address_record_shift);
    RUN_TEST(test_unknown_record_type);
    RUN_TEST(test_lowercase_hex_digits);
    RUN_TEST(test_longest_record_and_overlong_line);
    RUN_TEST(test_overlapping_or_descending_records_rejected);
    RUN_TEST(test_only_blank_lines_may_follow_eof);
    RUN_TEST(test_abort_refused_while_moving_or_applying);
    RUN_TEST(test_apply_gates_rechecked_when_due);
    RUN_TEST(test_motion_verbs_refused_while_apply_pending);
    return UNITY_END();
}
