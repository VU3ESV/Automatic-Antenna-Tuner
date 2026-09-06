#pragma once

// Browser control surface for the tuner controller — the initial means
// of operating the real tuner before the Pi master is deployed
// (CLAUDE.md "Stack": the master remains the long-term operator UI).
//
// Plain HTTP/1.1 on port 80, LAN-only, no auth (same posture as
// LP-100A-Server). GET verbs plus one POST for firmware upload:
//
//   GET /                serves the embedded one-page UI (web_page.h)
//   GET /api/status      full snapshot as JSON (polled by the page)
//   GET /api/<verb>?...  control verbs — every one of them calls the same
//                        app::motion entry point the master protocol
//                        uses, so the two surfaces can never disagree
//
// Verbs (axis = 0..2 or a bound element name such as L, C, C1, C2):
//   jog?axis&dir=cw|ccw&steps=N      bounded relative move (N=1 single step)
//   goto?axis&steps=N                bounded absolute move
//   rotate?axis&revs=N&dir=cw|ccw    ±N revolutions
//   run?axis&dir=cw|ccw|stop         run to the window end / stop
//   stop[?axis]                      immediate stop (one axis / all), no latch
//   estop[?axis]                     immediate stop + LATCHED alarm (one axis / all)
//   estop_reset[?axis]               release the alarm (one axis / all)
//   zero?axis   unhome?axis          declare / forget home
//   element?axis&kind&max_rev        element kind + rated travel
//   speed?axis&v[&acc]               cruise speed / ramp
//   enable?axis&on=1|0               driver ENA (setup only)
//   side?v=hi_z|lo_z                 K1/K2 (Balanced L)
//   bypass?on=1|0                    K3
//   home                             every bound axis back to 0
//   topology?kind=balanced_l&L=0&C=1 (or kind=balanced_pi&C1=0&L=1&C2=2)
//   fwd_w?w=N                        fake forward power (lockout test)
//   settings[?src=card]              config.json text: live record (default) or
//                                    the raw file read back from the card
//   settings_save                    write the card copy now
//   firmware                         firmware-update status JSON
//   POST firmware  (body = .hex)     stage a new image in free flash; 200 = staged + CRC
//   firmware_apply?lines=N           reboot into the staged image (N = staged record count);
//                                    needs bypass, no motion, no RF
//   firmware_abort                   discard the staged image (refused while moving)
//
// Replies: 200 text on success, 409 "<code>: <msg>" on a refusal,
// 400 on bad arguments (and on a rejected hex file), 408 when a firmware
// upload stalls, 405 for any method other than GET / POST. The page shows
// the reply on the axis card. /api/status carries a "build" object (compile
// time, git revision, env) and the "ota" status object.

#include "app/state.h"

namespace http_server {

constexpr uint16_t kPort = 80;

// Start listening. Call after Ethernet is up.
void begin();

// Poll for one request and serve it. `snap` is the latest snapshot
// (for /api/status); `master_clients` is shown in the page header.
void tick(const app::Snapshot &snap, int master_clients);

} // namespace http_server
