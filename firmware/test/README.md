# firmware/test — bench and bring-up projects

Standalone PlatformIO projects used to prove hardware before (or beside)
the production firmware in [`../tuner-controller/`](../tuner-controller/).
None of them is deployed; each has its own `platformio.ini` and is built
from its own directory.

| Project | Board | Purpose |
|---|---|---|
| [`teensy-selftest/`](teensy-selftest/) | Teensy 4.1 + PJRC Ethernet kit | Run first on a new Teensy: USB serial, LED, PHY link, DHCP, HTTP page. Builds against both Ethernet backends (`teensy41` = QNEthernet, `teensy41_native` = NativeEthernet). Built in CI. |
| [`t41-stepper-test/`](t41-stepper-test/) | Teensy 4.1 in the grblHAL V2.09 carrier | Three-axis stepper bench rig: serial menu + web UI, FlexPWM pulse trains, element kinds and software travel window, EEPROM persistence. Consumes the production pin map (`../tuner-controller/src/hal/board/t41_v209.h`) and the shared libraries in `../lib/`. |
| [`mega-stepper-test/`](mega-stepper-test/) | Arduino Mega 2560 + TB6600 | Original single-motor bench test (AccelStepper, loop-polled). Kept for reference. |
| [`esp32c6-stepper-test/`](esp32c6-stepper-test/) | ESP32-C6 DevKitC-1 + TB6600 | Bench test whose step loss under NVS / WiFi activity produced the "step pulses must be hardware-generated" rule in CLAUDE.md. Kept for reference. |

The **unit tests** for the production firmware are not here: they live in
[`../tuner-controller/test/`](../tuner-controller/test/) (PlatformIO's
`test/` convention) and run with `pio test -e native` from
`firmware/tuner-controller/`.

Build any project from its directory, e.g.

```sh
cd firmware/test/t41-stepper-test
pio run -e teensy41_native
```

Bench learnings that shaped the production firmware (TB6600 ENA
polarity, NativeEthernet write chunking, FlexPWM ISR double-trigger)
are recorded in [`../../PROPOSAL.md`](../../PROPOSAL.md) "Bench-test
learnings".
