#pragma once

// Tiny Ethernet abstraction over QNEthernet and NativeEthernet.
//
// Exactly one of the following must be defined via platformio.ini build_flags:
//
//   TUNER_NET_QNETHERNET       — Shawn Silverman's lwIP-based QNEthernet
//   TUNER_NET_NATIVEETHERNET   — vjmuzik's FNET-based NativeEthernet
//
// The two libraries differ in namespace, header, link-state and DHCP APIs.
// Callers should use net_hal::* and the `EthernetClient` / `EthernetServer`
// names exposed below, never the library types directly. That keeps the
// active backend a one-line change in platformio.ini.
//
// See docs/ARCHITECTURE.md §5.1.2 for the rationale and pros/cons.

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

#if defined(TUNER_NET_QNETHERNET)
    #include <QNEthernet.h>
    // QNEthernet wraps everything in qindesign::network — pull the
    // global `Ethernet` instance AND the type names into the global
    // namespace so call-site code is identical across both backends.
    using qindesign::network::Ethernet;
    using qindesign::network::EthernetClient;
    using qindesign::network::EthernetServer;
    using qindesign::network::EthernetUDP;
    #define TUNER_NET_LIB_NAME "QNEthernet"
#elif defined(TUNER_NET_NATIVEETHERNET)
    #include <NativeEthernet.h>
    #include <NativeEthernetUdp.h>
    // NativeEthernet's types are already global; nothing to import.
    #define TUNER_NET_LIB_NAME "NativeEthernet"
#else
    #error "Define TUNER_NET_QNETHERNET or TUNER_NET_NATIVEETHERNET via platformio.ini build_flags"
#endif

namespace net_hal {

// Bring up Ethernet with DHCP. Returns true if begin() didn't fail
// outright. On NativeEthernet, DHCP runs inside begin() and can block
// for up to ~60 s by default — use wait_dhcp() afterwards for parity
// with QNEthernet's asynchronous startup.
bool begin();

// Wait up to ms milliseconds for the PHY link. Returns true if link
// came up. On QNEthernet this is Ethernet.waitForLink(ms); on
// NativeEthernet we poll linkStatus().
bool wait_link(uint32_t ms);

// Wait up to ms milliseconds for a DHCP-assigned address.
bool wait_dhcp(uint32_t ms);

// Current link state.
bool link_state();

// Link speed in Mbps (10 or 100). Returns 0 when link is down or when
// the backend doesn't expose this (NativeEthernet on older versions).
int link_speed_mbps();

// True if link is full duplex. Returns false when link is down or when
// the backend doesn't expose this.
bool link_full_duplex();

// Reads the Teensy 4.x hardware MAC from the on-chip OTP fuse —
// library-independent so we always log the correct MAC even before
// Ethernet.begin() is called.
void hw_mac(uint8_t mac[6]);

// Backend identifier for logs / banners ("QNEthernet" / "NativeEthernet").
const char *lib_name();

// Write `n` bytes to a client in chunks of at most kWriteChunk, flushing
// between chunks. FNET's default per-socket send buffer is 2 KB and
// NativeEthernet's socketSend() busy-waits forever on a single write of
// ≥ that size; QNEthernet can return short under lwIP buffer pressure —
// or 0 when its send buffer is full, which is back-pressure, not a dead
// client (its write() services the stack, so a retry sees the peer's
// ACKs). One loop is correct on both backends: a 0-byte write is retried
// while the client is connected, for at most kWriteStallMs; on
// NativeEthernet 0 only happens on a dead socket, which connected() then
// reports. Returns false if the client went away or stalled mid-write.
// Callers must still flush() before stop(): NativeEthernet's stop()
// discards whatever is left in the send buffer. Bench findings in
// PROPOSAL.md "Bench-test learnings".
constexpr size_t   kWriteChunk   = 1024;
constexpr uint32_t kWriteStallMs = 2000;

inline bool write_all(EthernetClient &c, const void *buf, size_t n) {
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    size_t   sent          = 0;
    uint32_t last_progress = millis();
    while (sent < n) {
        if (!c.connected()) return false;
        size_t want = n - sent;
        if (want > kWriteChunk) want = kWriteChunk;
        const size_t w = c.write(p + sent, want);
        if (w == 0) {
            if (millis() - last_progress > kWriteStallMs) return false;
            delay(1);
            continue;
        }
        sent         += w;
        last_progress = millis();
        if (sent < n) c.flush();
    }
    return true;
}

} // namespace net_hal
