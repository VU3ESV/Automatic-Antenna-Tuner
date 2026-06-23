#ifdef TUNER_NET_QNETHERNET

#include "net_hal.h"

namespace net_hal {

bool begin() {
    return Ethernet.begin();
}

bool wait_link(uint32_t ms) {
    return Ethernet.waitForLink(ms);
}

bool wait_dhcp(uint32_t ms) {
    return Ethernet.waitForLocalIP(ms);
}

bool link_state() {
    return Ethernet.linkState();
}

int link_speed_mbps() {
    if (!Ethernet.linkState()) return 0;
    return Ethernet.linkInfo().speed;
}

bool link_full_duplex() {
    if (!Ethernet.linkState()) return false;
    return Ethernet.linkInfo().fullNotHalfDuplex;
}

void hw_mac(uint8_t mac[6]) {
    Ethernet.macAddress(mac);
}

const char *lib_name() {
    return TUNER_NET_LIB_NAME;
}

} // namespace net_hal

#endif // TUNER_NET_QNETHERNET
