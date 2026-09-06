// Tuner controller — Teensy 4.1 on the grblHAL V2.09 carrier.
//
// Boot order (each step is visible on the USB serial console):
//   [0]  K3 latched into BYPASS before anything else (invariant 2) —
//        app::motion::init() does this first, then restores the persisted
//        topology, element config and position anchors (invariant 3) and
//        enables the drives.
//   [1]  USB serial, LED solid.
//   [2]  Ethernet PHY link + DHCP via the configured net_hal backend.
//   [3]  Master link: TCP line-JSON server on port 8089 (docs/PROTOCOL.md).
//   [4]  Browser control: HTTP server + embedded page on port 80.
//   [5]  Loop: service motion, publish the snapshot, tick both servers.
//
// Nothing moves at boot. Unlike the bench rig there is no drive-to-zero
// on power-up: positions come back from EEPROM as anchors and the
// operator (or the master) commands every move.

#include <Arduino.h>

#include "app/motion.h"
#include "app/settings.h"
#include "app/state.h"
#include "hal/hal.h"
#include "http_server.h"
#include "net_hal.h"
#include "tuner_server.h"

namespace {

constexpr uint32_t LINK_TIMEOUT_MS       = 15000;
constexpr uint32_t DHCP_TIMEOUT_MS       = 15000;
constexpr uint32_t BLINK_LINK_PERIOD_MS  = 500;   // link up, no DHCP yet
constexpr uint32_t BLINK_READY_PERIOD_MS = 100;   // servers live
constexpr uint32_t STATUS_PERIOD_MS      = 10000;

#define IP_FMT      "%u.%u.%u.%u"
#define IP_ARG(ip)  (ip)[0], (ip)[1], (ip)[2], (ip)[3]

unsigned long lastBlinkMs  = 0;
unsigned long lastStatusMs = 0;
bool          linkUp       = false;
bool          dhcpOK       = false;
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
}

void print_config() {
    const app::settings::Status &st = app::settings::status();
    Serial.printf("Settings: %s (SD card %s%s)\n",
                  st.source == app::SettingsSource::Sd ? "from SD card" :
                  st.source == app::SettingsSource::Eeprom ? "from EEPROM" : "defaults (blank store)",
                  st.sd_present ? "present" : "absent",
                  st.sd_present ? (st.sd_ok ? ", config.json ok" : ", config.json NOT written") : "");
    const app::Topology &t = app::motion::topology();
    Serial.printf("Topology: %s\n", app::topology_kind_name(t.kind));
    for (uint8_t i = 0; i < t.n; i++) {
        const app::ElementBinding &b = t.elements[i];
        Serial.printf("  %-2s (%s%s) -> motor %u\n", b.name, b.type == app::ElementType::L ? "L" : "C",
                      b.pair ? ", pair" : "", b.axis);
    }
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        const app::AxisConfig &c = app::motion::axis_config(a);
        Serial.printf("Motor %u: pos %ld, %s", a, static_cast<long>(hal::motor::position(a)),
                      app::element_kind_name(c.kind));
        if (app::kind_has_stops(c.kind)) {
            Serial.printf(", rated %.3f rev (%ld steps), home %s, window %s",
                          static_cast<double>(c.max_rev), static_cast<long>(c.max_steps()),
                          c.home_set ? "SET" : "NOT SET", c.limits_active() ? "ACTIVE" : "inactive");
        }
        Serial.printf(", %lu steps/s, %lu steps/s2\n",
                      static_cast<unsigned long>(c.speed), static_cast<unsigned long>(c.accel));
    }
}

} // namespace

void setup() {
    // [0] Relays + config + drives first. Bypass is latched inside
    // motion::init() before any other peripheral is touched.
    app::motion::init();

    pinMode(LED_BUILTIN, OUTPUT);
    hal::led::init();
    hal::led::set(true);

    Serial.begin(115200);
    const unsigned long serialStart = millis();
    while (!Serial && (millis() - serialStart) < 3000) {}

    Serial.println();
    Serial.println("===========================================");
    Serial.println("Automatic Antenna Tuner — controller");
    Serial.printf("Net backend: %s\n", net_hal::lib_name());
    Serial.println("===========================================");
    Serial.println("[0] K3 BYPASS latched; drives enabled; config restored from EEPROM:");
    print_config();
    Serial.println("[1] Serial up.");
    print_mac();

    Serial.printf("[2] Bringing up Ethernet via %s...\n", net_hal::lib_name());
    if (!net_hal::begin()) Serial.println("    !! net_hal::begin() returned false. Check PHY/cable.");
    Serial.print("    waiting for link...");
    linkUp = net_hal::wait_link(LINK_TIMEOUT_MS);
    Serial.println(linkUp ? " up" : " TIMED OUT");
    Serial.println("    waiting for DHCP lease...");
    if (net_hal::wait_dhcp(DHCP_TIMEOUT_MS)) {
        dhcpOK = true;
        print_net();
    } else {
        Serial.println("    !! DHCP timed out. Motion still works from the master once the link returns.");
    }

    if (dhcpOK) {
        tuner_server::begin();
        Serial.printf("[3] Master link: TCP :%u  (set master [tuner].host = \"" IP_FMT "\")\n",
                      tuner_server::kListenPort, IP_ARG(Ethernet.localIP()));
        http_server::begin();
        Serial.printf("[4] Browser UI:  http://" IP_FMT "/\n", IP_ARG(Ethernet.localIP()));
    }

    Serial.println("===========================================");
    Serial.println(linkUp && dhcpOK ? "ALL GREEN. LED fast-blink @ 5 Hz." : "PARTIAL. See messages above.");
    Serial.println();
}

void loop() {
    const unsigned long now = millis();

    if (linkUp) {
        const uint32_t period = dhcpOK ? BLINK_READY_PERIOD_MS : BLINK_LINK_PERIOD_MS;
        if ((now - lastBlinkMs) >= period) {
            lastBlinkMs = now;
            hal::led::toggle();
        }
    }

    // Motion is serviced whether or not the network is up.
    app::motion::tick(now, snapshot);

    if (dhcpOK) {
        tuner_server::publish(snapshot);
        tuner_server::tick();
        http_server::tick(snapshot, tuner_server::connected_clients());
    }

    if ((now - lastStatusMs) >= STATUS_PERIOD_MS) {
        lastStatusMs = now;
        Serial.printf("[%6lus] link=%s ip=" IP_FMT " master_clients=%d bypass=%d homed=%d pos=%ld/%ld/%ld\n",
                      now / 1000UL, net_hal::link_state() ? "up" : "down", IP_ARG(Ethernet.localIP()),
                      tuner_server::connected_clients(), snapshot.bypass, snapshot.homed,
                      static_cast<long>(snapshot.axes[0].steps), static_cast<long>(snapshot.axes[1].steps),
                      static_cast<long>(snapshot.axes[2].steps));
    }
}
