#ifdef TUNER_NET_NATIVEETHERNET

#include "net_hal.h"

// HW_OCOTP_MAC0 / MAC1 are i.MX RT1062 OTP fuse registers holding the
// factory-programmed Teensy MAC. NativeEthernet's begin() needs the MAC
// passed in explicitly, unlike QNEthernet which fetches it internally.

namespace net_hal {

void hw_mac(uint8_t mac[6]) {
    // Layout per Teensyduino reference: MAC1 holds the upper 2 bytes,
    // MAC0 holds the lower 4 bytes.
    for (uint8_t i = 0; i < 2; i++) {
        mac[i]     = (HW_OCOTP_MAC1 >> ((1 - i) * 8)) & 0xFF;
    }
    for (uint8_t i = 0; i < 4; i++) {
        mac[i + 2] = (HW_OCOTP_MAC0 >> ((3 - i) * 8)) & 0xFF;
    }
}

bool begin() {
    uint8_t mac[6];
    hw_mac(mac);
    // NativeEthernet's begin(mac) returns int (0 = DHCP failed, 1 = OK).
    // DHCP attempt is synchronous inside begin().
    return Ethernet.begin(mac) != 0;
}

bool wait_link(uint32_t ms) {
    const uint32_t start = millis();
    while (Ethernet.linkStatus() != LinkON) {
        if ((millis() - start) >= ms) return false;
        delay(50);
    }
    return true;
}

bool wait_dhcp(uint32_t ms) {
    // NativeEthernet acquires DHCP inside begin(); if begin() succeeded
    // we already have an IP. This polls in case begin() was called
    // without waiting, to keep the API symmetric with QNEthernet.
    const IPAddress zero(0, 0, 0, 0);
    const uint32_t start = millis();
    while (Ethernet.localIP() == zero) {
        if ((millis() - start) >= ms) return false;
        delay(50);
    }
    return true;
}

bool link_state() {
    return Ethernet.linkStatus() == LinkON;
}

int link_speed_mbps() {
    // NativeEthernet does not expose link speed in a portable way across
    // versions. Report 0 (= "unknown") rather than guessing.
    return 0;
}

bool link_full_duplex() {
    // Likewise — duplex isn't exposed; report false ("unknown").
    return false;
}

const char *lib_name() {
    return TUNER_NET_LIB_NAME;
}

} // namespace net_hal

#endif // TUNER_NET_NATIVEETHERNET
