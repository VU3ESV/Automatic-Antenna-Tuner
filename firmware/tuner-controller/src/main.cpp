// Tuner controller — M1b firmware.
//
// Boot order:
//   [1/4]  USB serial, LED solid until network is up.
//   [2/4]  Ethernet PHY link + DHCP via the configured net_hal backend
//          (QNEthernet or NativeEthernet — selected at compile time).
//   [3/4]  TCP server on port 8089 (docs/PROTOCOL.md §1.0).
//   [4/4]  Idle loop: blink LED, publish snapshot, tick the server.
//
// What's NOT here yet: motor control, encoder reading, relay control,
// safety lockout. Those hook into app::Snapshot via published()
// once the corresponding HAL implementations land in M1b hardware
// integration. The protocol surface is live now so the master and
// browser can render real Teensy-sourced state.

#include <Arduino.h>

#include "app/state.h"
#include "hal/hal.h"
#include "net_hal.h"
#include "tuner_server.h"

namespace {

constexpr uint32_t LINK_TIMEOUT_MS       = 15000;
constexpr uint32_t DHCP_TIMEOUT_MS       = 15000;
constexpr uint32_t BLINK_LINK_PERIOD_MS  = 500;  // link up, no DHCP yet
constexpr uint32_t BLINK_READY_PERIOD_MS = 100;  // DHCP + server live
constexpr uint32_t STATUS_PERIOD_MS      = 10000;

#define IP_FMT      "%u.%u.%u.%u"
#define IP_ARG(ip)  (ip)[0], (ip)[1], (ip)[2], (ip)[3]

unsigned long lastBlinkMs   = 0;
unsigned long lastStatusMs  = 0;
bool          linkUp        = false;
bool          dhcpOK        = false;
app::Snapshot snapshot;

void print_mac() {
    uint8_t mac[6];
    net_hal::hw_mac(mac);
    Serial.printf("MAC:     %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void print_net() {
    Serial.printf("IP:      " IP_FMT "\n", IP_ARG(Ethernet.localIP()));
    Serial.printf("Mask:    " IP_FMT "\n", IP_ARG(Ethernet.subnetMask()));
    Serial.printf("Gateway: " IP_FMT "\n", IP_ARG(Ethernet.gatewayIP()));
    Serial.printf("DNS:     " IP_FMT "\n", IP_ARG(Ethernet.dnsServerIP()));
}

} // namespace

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    hal::led::init();
    hal::led::set(true);

    Serial.begin(115200);
    const unsigned long serialStart = millis();
    while (!Serial && (millis() - serialStart) < 3000) {}

    Serial.println();
    Serial.println("===========================================");
    Serial.println("Tuner Controller — M1b firmware");
    Serial.printf("Net backend: %s\n", net_hal::lib_name());
    Serial.println("===========================================");

    Serial.println("[1/4] Booted; LED solid; serial up.");
    print_mac();

    Serial.printf("[2/4] Bringing up Ethernet via %s...\n", net_hal::lib_name());
    if (!net_hal::begin()) {
        Serial.println("       !! net_hal::begin() returned false. Check PHY/cable.");
    }

    Serial.print("       waiting for link...");
    linkUp = net_hal::wait_link(LINK_TIMEOUT_MS);
    Serial.println(linkUp ? " up" : " TIMED OUT");

    if (linkUp) {
        const int speed = net_hal::link_speed_mbps();
        if (speed > 0) {
            Serial.printf("       link: %d Mbps, %s\n", speed,
                          net_hal::link_full_duplex() ? "full-duplex" : "half-duplex");
        } else {
            Serial.println("       link: up");
        }
    }

    Serial.println("[3/4] Waiting for DHCP lease...");
    if (net_hal::wait_dhcp(DHCP_TIMEOUT_MS)) {
        dhcpOK = true;
        print_net();
    } else {
        Serial.println("       !! DHCP timed out.");
    }

    if (dhcpOK) {
        tuner_server::begin();
        Serial.printf("[4/4] TCP protocol server listening on port %u\n",
                      tuner_server::kListenPort);
        Serial.printf("       set master `[tuner].host = \"" IP_FMT
                      "\"` and `[tuner].port = %u`\n",
                      IP_ARG(Ethernet.localIP()),
                      tuner_server::kListenPort);
    }

    Serial.println("===========================================");
    if (linkUp && dhcpOK) {
        Serial.println("ALL GREEN. LED fast-blink @ 5 Hz; awaiting master.");
    } else {
        Serial.println("PARTIAL. See [n/4] messages above.");
    }
    Serial.println();

    snapshot.bypass = true;  // invariant #2: bypass on power-up.
    snapshot.side   = app::Side::HiZ;
}

void loop() {
    const unsigned long now = millis();

    // LED progress encoding (matches selftest: solid → 1 Hz → 5 Hz).
    if (linkUp) {
        const uint32_t period = dhcpOK ? BLINK_READY_PERIOD_MS : BLINK_LINK_PERIOD_MS;
        if ((now - lastBlinkMs) >= period) {
            lastBlinkMs = now;
            hal::led::toggle();
        }
    }

    if (dhcpOK) {
        // Publish the current snapshot so any state-change (when hardware
        // wiring lands) propagates to clients. For now, the snapshot is
        // static after boot — heartbeats keep the link alive.
        tuner_server::publish(snapshot);
        tuner_server::tick();
    }

    if ((now - lastStatusMs) >= STATUS_PERIOD_MS) {
        lastStatusMs = now;
        Serial.printf("[%6lus] backend=%s link=%s ip=" IP_FMT " clients=%d\n",
                      now / 1000UL,
                      net_hal::lib_name(),
                      net_hal::link_state() ? "up" : "down",
                      IP_ARG(Ethernet.localIP()),
                      tuner_server::connected_clients());
    }
}
