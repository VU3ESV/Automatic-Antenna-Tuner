#ifdef TUNER_NET_QNETHERNET

#include "net_hal.h"

#include <cstring>

namespace net_hal {

namespace {
char g_hostname[64] = "";

void remember(const char *name) {
    if (!name) return;
    strncpy(g_hostname, name, sizeof(g_hostname) - 1);
    g_hostname[sizeof(g_hostname) - 1] = '\0';
}
} // namespace

bool begin(const char *hostname) {
    remember(hostname);
    // Must precede begin(): the name goes out in the first DHCP DISCOVER.
    if (g_hostname[0]) Ethernet.setHostname(g_hostname);
    return Ethernet.begin();
}

bool dhcp_sends_hostname() { return true; }

bool start_mdns(const char *hostname, uint16_t http_port) {
    remember(hostname);
    if (!g_hostname[0]) return false;
    if (!qindesign::network::MDNS.begin(g_hostname)) return false;
    return qindesign::network::MDNS.addService("_http", "_tcp", http_port);
}

const char *hostname() { return g_hostname; }

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
