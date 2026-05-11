#ifdef TARGET_NATIVE

#include "hal/hal.h"
#include <cstdio>

namespace hal::led {

static bool state = false;

void init()           { state = false; }
void set(bool on)     { state = on; std::printf("[led] %s\n", on ? "ON" : "OFF"); }
void toggle()         { set(!state); }

} // namespace hal::led

#endif
