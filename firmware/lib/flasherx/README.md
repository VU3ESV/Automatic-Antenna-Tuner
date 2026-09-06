# flasherx — vendored FlasherX flash layer

`src/FlashTxx.c` / `src/FlashTxx.h` come from
[joepasquariello/FlasherX](https://github.com/joepasquariello/FlasherX)
(commit `9d5067b`, 2022-10-15). The files are marked **public domain** by
their authors (Niels A. Moseley, Jon Zeeff, Deb Hollenback, Frank Boesing,
Joe Pasquariello; Teensy 4 flash routines by Paul Stoffregen from the
Teensyduino core). Keep the header attribution when updating.

Only the flash layer is vendored. FlasherX's Intel-HEX reader
(`FXUtil.cpp`) is interactive (types the line count back on a serial
console) and reads its stream with no timeout, so the tuner-controller
has its own hex parser in `firmware/tuner-controller/src/app/ota.cpp`,
unit-tested on the native target, and drives this layer through
`hal::firmware` (`hal/firmware_teensy41.cpp`).

## Local patches (search the sources for "Automatic-Antenna-Tuner patch")

1. **`FLASH_RESERVE` for the Teensy 4.1 = 64 sectors (256 KB), was 4.**
   Teensyduino's EEPROM emulation owns the top 256 KB of the 8 MB flash
   (`cores/teensy4/eeprom.c`: `FLASH_BASEADDR 0x607C0000`, 63 sectors).
   The tuner keeps its settings and position anchors there. With the
   upstream value `firmware_buffer_init()` would place the staging buffer
   inside that region and `flash_move()` / `firmware_buffer_free()` would
   erase it. `hal/firmware_teensy41.cpp` carries a `static_assert` on this.
2. **`flash_move()` programs 256-byte pages on the i.MX RT1062**, not
   4-byte words. Page-program time on the W25Q64 does not depend on the
   byte count, so the ~66 000 word programs a 270 KB image needed
   upstream (~25 s) become ~1 060 page programs (~0.5 s). The copy is now
   dominated by the sector erases — 67 for the program area plus 67 for
   the buffer, 45 ms typical / 400 ms max each — so the whole
   cannot-lose-power window is several seconds typical, tens of seconds
   worst case. Sector erase-before-write is unchanged (pages never
   straddle a 4 KB sector). The same block aligns `offset` up to a word
   before the post-copy buffer erase, which steps by 4 and would never see
   a sector boundary for an image whose size is not a multiple of 4.
3. **Interrupts stay masked for the whole `flash_move()` on the i.MX RT.**
   The core's `flash_wait()` ends with `__enable_irq()`, so upstream only
   had them off inside each erase / program while the sector holding the
   FLASHMEM / PROGMEM data (USB descriptors) was already erased.
   `__disable_irq()` is now asserted at entry and after every core call;
   everything the routine touches is in ITCM / DTCM.
4. **`FLASH_ID` for the Teensy 4.1 is `fw_aat_teensy41`** (upstream
   `fw_teensy41`), so an unrelated Teensy 4.1 sketch that also links
   FlasherX is refused by `check_flash_id()`, not just an image built
   without the update code.

Everything else is upstream, including the 4-byte `flash_write_block()`
(unused here — the HAL stages pages itself) and the Kinetis (Teensy 3.x)
paths, which this project does not build.

## How the tuner uses it

```
firmware_buffer_init()  → free flash between the program and FLASH_RESERVE
eepromemu_flash_write() → stage the new image page by page (HAL)
check_flash_id()        → the new image must contain "fw_aat_teensy41"
flash_move()            → erase + copy over the program, then reboot
firmware_buffer_free()  → erase the buffer on abort (never while an axis moves)
flash_erase_block()     → reclaim a stage abandoned by a reboot (HAL, before buffer_init)
```

The controller side is documented in `docs/PROTOCOL.md` ("Firmware update
over Ethernet") and the uploader is
`firmware/tuner-controller/tools/ota_upload.py`.
