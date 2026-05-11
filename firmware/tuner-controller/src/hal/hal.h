#pragma once

// HAL interface — all platform-specific peripheral access goes through
// these namespaces. Implementations are split per-target under hal/ and
// gated by TARGET_TEENSY41 / TARGET_STM32H743 / TARGET_NATIVE macros
// driven from platformio.ini build_flags.
//
// Application logic (src/app/) MUST NOT include platform headers
// directly. If you need a peripheral that isn't exposed here yet, add
// it to this file first, then implement all three targets.

namespace hal {

namespace led {
    void init();
    void set(bool on);
    void toggle();
}

} // namespace hal
