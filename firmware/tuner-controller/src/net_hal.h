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

} // namespace net_hal
