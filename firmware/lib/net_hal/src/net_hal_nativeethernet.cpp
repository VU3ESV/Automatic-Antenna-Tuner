#ifdef TUNER_NET_NATIVEETHERNET

#include "net_hal.h"

#include <cstring>

// FNET's DHCP client sends this as the Host Name option (12) — but only in a
// project that patches FNET at build time
// (firmware/tuner-controller/tools/fnet_dhcp_hostname.py defines it). Weak so
// other net_hal users still link; its address is then null.
extern "C" const char *fnet_dhcp_cln_hostname __attribute__((weak));

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

namespace {
// At most 63 characters (one DNS label). FNET copies the name unchecked into
// 77-byte host and service name buffers (FNET_MDNS_NAME_MAX 64 plus room for
// a conflict counter), so this bound is what keeps those copies in range.
char g_hostname[64] = "";

void remember(const char *name) {
    if (!name) return;
    strncpy(g_hostname, name, sizeof(g_hostname) - 1);
    g_hostname[sizeof(g_hostname) - 1] = '\0';
}

// Empty TXT record set: FNET walks the table to its all-zero entry.
const fnet_mdns_txt_key_t *no_txt() {
    static const fnet_mdns_txt_key_t end[] = {{nullptr, nullptr}};
    return end;
}
} // namespace

bool begin(const char *hostname) {
    remember(hostname);
    // Must precede Ethernet.begin(): DHCP runs inside it, and renewals keep
    // using the pointer (static storage).
    if (&fnet_dhcp_cln_hostname != nullptr && g_hostname[0]) fnet_dhcp_cln_hostname = g_hostname;
    uint8_t mac[6];
    hw_mac(mac);
    // NativeEthernet's begin(mac) returns int (0 = DHCP failed, 1 = OK).
    // DHCP attempt is synchronous inside begin().
    return Ethernet.begin(mac) != 0;
}

bool dhcp_sends_hostname() { return &fnet_dhcp_cln_hostname != nullptr; }

bool start_mdns(const char *hostname, uint16_t http_port) {
    remember(hostname);
    if (!g_hostname[0]) return false;
    // Straight to FNET, not NativeEthernet's MDNS wrapper: the wrapper's
    // addService() leaves fnet_mdns_service_t::name uninitialised, and
    // fnet_mdns_service_register() strcpy()s from that pointer before any
    // check (on the bench it announced an unnamed _http._tcp instance; a
    // different stack value could fault or overrun the name buffer). The
    // responder then runs from NativeEthernet's FNET poll timer.
    fnet_mdns_params_t params;
    memset(&params, 0, sizeof(params));
    params.netif_desc  = fnet_netif_get_default();
    params.addr_family = AF_INET;
    params.rr_ttl      = FNET_CFG_MDNS_RR_TTL;
    params.name        = g_hostname;   // copied by fnet_mdns_init()
    const fnet_mdns_desc_t mdns = fnet_mdns_init(&params);
    if (!mdns) return false;

    fnet_mdns_service_t http;
    memset(&http, 0, sizeof(http));
    http.name            = g_hostname;     // the instance name: "tuner-controller._http._tcp.local"
    http.service_type    = "_http._tcp";   // FNET keeps this pointer — a literal
    http.service_port    = FNET_HTONS(http_port);
    http.service_get_txt = no_txt;
    return fnet_mdns_service_register(mdns, &http) != 0;
}

const char *hostname() { return g_hostname; }

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
