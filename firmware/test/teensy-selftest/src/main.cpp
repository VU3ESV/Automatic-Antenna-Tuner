// Teensy 4.1 + Ethernet selftest.
//
// Verifies, in order:
//   [1/4]  Board boots, USB serial works, LED is wired (solid until link).
//   [2/4]  Ethernet PHY is alive and the cable carries link state.
//   [3/4]  DHCP succeeds → we have IP / mask / gateway / DNS.
//   [4/4]  TCP server listens on :80 and serves an HTML status page.
//
// LED encodes progress without needing the serial monitor:
//   solid           = booting / no link
//   slow blink 1 Hz = link up, waiting for DHCP
//   fast blink 5 Hz = DHCP acquired, fully ready
//
// The Ethernet library backend is selected at compile time via
// platformio.ini build_flags (TUNER_NET_QNETHERNET or
// TUNER_NET_NATIVEETHERNET). The same main.cpp compiles against either.
// See net_hal.h and docs/ARCHITECTURE.md §5.1.2.

#include <Arduino.h>

#include "net_hal.h"

// ---- Configuration --------------------------------------------------------

static constexpr uint16_t HTTP_PORT             = 80;
static constexpr uint32_t LINK_TIMEOUT_MS       = 15000;
static constexpr uint32_t DHCP_TIMEOUT_MS       = 15000;
// LED rates (toggle period; visible blink rate is half):
//   solid                    = pre-link
//   BLINK_LINK_PERIOD_MS     = link up, no DHCP yet           (1 Hz blink)
//   BLINK_READY_PERIOD_MS    = DHCP succeeded, fully ready    (5 Hz blink)
static constexpr uint32_t BLINK_LINK_PERIOD_MS  = 500;
static constexpr uint32_t BLINK_READY_PERIOD_MS = 100;
static constexpr uint32_t STATUS_PERIOD_MS      = 10000;  // serial heartbeat

// ---- Runtime state --------------------------------------------------------

static EthernetServer server(HTTP_PORT);
static uint8_t        mac[6]          = {0};
static unsigned long  lastBlinkMs     = 0;
static unsigned long  lastStatusMs    = 0;
static unsigned long  requestsServed  = 0;
static bool           linkUp          = false;
static bool           dhcpOK          = false;

// ---- Helpers --------------------------------------------------------------

// Teensyduino's IPAddress has no toString(); format with these macros.
#define IP_FMT      "%u.%u.%u.%u"
#define IP_ARG(ip)  (ip)[0], (ip)[1], (ip)[2], (ip)[3]

static void printBanner() {
    Serial.println();
    Serial.println("===========================================");
    Serial.println("Teensy 4.1 + Ethernet selftest");
    Serial.println("Automatic Antenna Tuner / firmware bring-up");
    Serial.printf("Ethernet backend: %s\n", net_hal::lib_name());
    Serial.println("===========================================");
}

static void printMAC() {
    net_hal::hw_mac(mac);
    Serial.printf("MAC:     %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void printNetInfo() {
    Serial.printf("IP:      " IP_FMT "\n", IP_ARG(Ethernet.localIP()));
    Serial.printf("Mask:    " IP_FMT "\n", IP_ARG(Ethernet.subnetMask()));
    Serial.printf("Gateway: " IP_FMT "\n", IP_ARG(Ethernet.gatewayIP()));
    Serial.printf("DNS:     " IP_FMT "\n", IP_ARG(Ethernet.dnsServerIP()));
}

static void serveHTTP(EthernetClient &client) {
    // Drain whatever request bytes are pending (we don't parse paths).
    const unsigned long start = millis();
    while (client.connected() && client.available() == 0 && (millis() - start) < 500) {
        delay(1);
    }
    while (client.available()) {
        (void)client.read();
    }

    const IPAddress ip = Ethernet.localIP();

    char body[1280];
    const int n = snprintf(
        body, sizeof(body),
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>Teensy 4.1 selftest</title>"
        "<style>body{font-family:system-ui,-apple-system,sans-serif;"
        "background:#111;color:#eee;margin:0;padding:2em;line-height:1.4}"
        "h1{font-weight:300;margin:0 0 .25em}.ok{color:#4f4}"
        ".panel{padding:.75em 1em;border:1px solid #333;border-radius:.5em;"
        "background:#1a1a1a;margin-top:.75em}"
        ".label{color:#888;font-size:.8em}.val{font-size:1.2em;font-family:ui-monospace,Menlo,monospace}"
        "</style></head><body>"
        "<h1>Teensy 4.1 <span class=\"ok\">healthy</span></h1>"
        "<div class=\"panel\"><div class=\"label\">Ethernet backend</div>"
        "<div class=\"val\">%s</div></div>"
        "<div class=\"panel\"><div class=\"label\">MAC</div>"
        "<div class=\"val\">%02X:%02X:%02X:%02X:%02X:%02X</div></div>"
        "<div class=\"panel\"><div class=\"label\">IP</div>"
        "<div class=\"val\">%u.%u.%u.%u</div></div>"
        "<div class=\"panel\"><div class=\"label\">Uptime</div>"
        "<div class=\"val\">%lu s</div></div>"
        "<div class=\"panel\"><div class=\"label\">Requests served</div>"
        "<div class=\"val\">%lu</div></div>"
        "<div class=\"panel\"><div class=\"label\">Ethernet link</div>"
        "<div class=\"val\">%s</div></div>"
        "<p style=\"color:#888;margin-top:2em\">Automatic Antenna Tuner / Teensy selftest</p>"
        "</body></html>",
        net_hal::lib_name(),
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        ip[0], ip[1], ip[2], ip[3],
        millis() / 1000UL,
        requestsServed + 1,
        net_hal::link_state() ? "up" : "down");

    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Content-Type: text/html; charset=utf-8\r\n");
    client.printf("Content-Length: %d\r\n", n);
    client.print("Connection: close\r\n\r\n");
    client.write(reinterpret_cast<const uint8_t *>(body), n);
}

// ---- Setup / loop ---------------------------------------------------------

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);  // solid until link is up

    Serial.begin(115200);
    const unsigned long serialStart = millis();
    while (!Serial && (millis() - serialStart) < 3000) {
        // Wait up to 3 s for USB CDC so the banner is captured.
    }

    printBanner();
    Serial.println("[1/4] Booted; LED solid; serial up.");
    printMAC();

    Serial.printf("[2/4] Bringing up Ethernet via %s (DHCP, %lu ms link timeout)...\n",
                  net_hal::lib_name(), (unsigned long)LINK_TIMEOUT_MS);
    if (!net_hal::begin()) {
        Serial.println("       !! net_hal::begin() returned false. Check PHY/cable.");
    }

    Serial.print("       waiting for link...");
    linkUp = net_hal::wait_link(LINK_TIMEOUT_MS);
    Serial.println(linkUp ? " up" : " TIMED OUT");

    if (linkUp) {
        const int speed = net_hal::link_speed_mbps();
        const bool fdx  = net_hal::link_full_duplex();
        if (speed > 0) {
            Serial.printf("       link: %d Mbps, %s\n",
                          speed, fdx ? "full-duplex" : "half-duplex");
        } else {
            // NativeEthernet doesn't expose this; report what we do know.
            Serial.println("       link: up (speed/duplex not reported by backend)");
        }
    } else {
        Serial.println("       !! No PHY link. Cable unplugged? Switch off? Auto-negotiation failed?");
    }

    Serial.println("[3/4] Waiting for DHCP lease...");
    if (net_hal::wait_dhcp(DHCP_TIMEOUT_MS)) {
        dhcpOK = true;
        printNetInfo();
    } else {
        Serial.println("       !! DHCP timed out. No DHCP server? Wrong VLAN?");
    }

    server.begin();
    Serial.printf("[4/4] HTTP server listening on port %u\n", HTTP_PORT);
    if (dhcpOK) {
        Serial.printf("       open  http://" IP_FMT "/  from another device on the LAN\n",
                      IP_ARG(Ethernet.localIP()));
    }

    Serial.println("===========================================");
    if (linkUp && dhcpOK) {
        Serial.println("ALL GREEN. LED fast-blink @ 5 Hz; status heartbeat every 10 s.");
    } else if (linkUp) {
        Serial.println("PARTIAL: link up but no DHCP. LED slow-blink @ 1 Hz.");
    } else {
        Serial.println("PARTIAL: no link. LED solid. See [n/4] messages above.");
    }
    Serial.println();
}

void loop() {
    const unsigned long now = millis();

    // LED rate signals progress: solid pre-link → 1 Hz on link → 5 Hz on
    // DHCP. dhcpOK can only flip true if linkUp is true, so this also
    // gates "fast" on link being present.
    if (linkUp) {
        const uint32_t period = dhcpOK ? BLINK_READY_PERIOD_MS : BLINK_LINK_PERIOD_MS;
        if ((now - lastBlinkMs) >= period) {
            lastBlinkMs = now;
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        }
    }

    // Periodic one-line serial heartbeat.
    if ((now - lastStatusMs) >= STATUS_PERIOD_MS) {
        lastStatusMs = now;
        Serial.printf("[%6lus] backend=%s link=%s ip=" IP_FMT " requests=%lu\n",
                      now / 1000UL,
                      net_hal::lib_name(),
                      net_hal::link_state() ? "up" : "down",
                      IP_ARG(Ethernet.localIP()),
                      requestsServed);
    }

    // Accept any waiting HTTP client.
    EthernetClient client = server.accept();
    if (client) {
        Serial.printf("[http] " IP_FMT ":%u\n",
                      IP_ARG(client.remoteIP()),
                      client.remotePort());
        serveHTTP(client);
        requestsServed++;
        client.stop();
    }
}
