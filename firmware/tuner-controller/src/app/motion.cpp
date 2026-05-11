#include "app/motion.h"

#include "hal/hal.h"

namespace app::motion {

namespace {

bool prev_busy_l = false;
bool prev_busy_c = false;
uint32_t last_move_ms_l = 0;
uint32_t last_move_ms_c = 0;
bool homed_flag = false;

hal::SideSel to_hal(Side s) {
    return s == Side::HiZ ? hal::SideSel::HiZ : hal::SideSel::LoZ;
}

Side from_hal(hal::SideSel s) {
    return s == hal::SideSel::HiZ ? Side::HiZ : Side::LoZ;
}

bool refuse_if_rf(Refusal &out_err) {
    if (hal::safety::rf_present()) {
        out_err.code = "rf_lockout";
        out_err.msg  = "fwd_w exceeds tx_lockout_w";
        return true;
    }
    return false;
}

} // namespace

void init() {
    hal::motor::init();
    hal::encoder::init();
    hal::relay::init();
    hal::safety::init();
    prev_busy_l = false;
    prev_busy_c = false;
    last_move_ms_l = 0;
    last_move_ms_c = 0;
    homed_flag = false;
}

void tick(uint32_t now_ms, Snapshot &out) {
    hal::motor::tick();

    const bool busy_l = hal::motor::busy(hal::Axis::L);
    const bool busy_c = hal::motor::busy(hal::Axis::C);

    // Falling edge on either axis stamps last_move.
    if (prev_busy_l && !busy_l) last_move_ms_l = now_ms;
    if (prev_busy_c && !busy_c) last_move_ms_c = now_ms;
    prev_busy_l = busy_l;
    prev_busy_c = busy_c;

    out.l_steps = static_cast<uint32_t>(hal::motor::position(hal::Axis::L));
    out.c_steps = static_cast<uint32_t>(hal::motor::position(hal::Axis::C));
    out.l_enc   = hal::encoder::count(hal::Axis::L);
    out.c_enc   = hal::encoder::count(hal::Axis::C);
    out.side    = from_hal(hal::relay::side());
    out.bypass  = hal::relay::bypass();
    out.moving  = busy_l || busy_c;
    out.homed   = homed_flag;
    out.last_move_ms = (last_move_ms_l > last_move_ms_c)
                          ? last_move_ms_l : last_move_ms_c;

    out.fwd_w = hal::safety::fwd_w();
}

bool move_l(int32_t value, bool is_delta, Refusal &out_err) {
    if (refuse_if_rf(out_err)) return false;
    if (is_delta) hal::motor::move_by(hal::Axis::L, value);
    else          hal::motor::move_to(hal::Axis::L, value);
    return true;
}

bool move_c(int32_t value, bool is_delta, Refusal &out_err) {
    if (refuse_if_rf(out_err)) return false;
    if (is_delta) hal::motor::move_by(hal::Axis::C, value);
    else          hal::motor::move_to(hal::Axis::C, value);
    return true;
}

bool set_side(Side s, Refusal &out_err) {
    if (refuse_if_rf(out_err)) return false;
    hal::relay::set_side(to_hal(s));
    return true;
}

bool set_bypass(bool on, Refusal &out_err) {
    (void)out_err;
    // Invariant: bypass is the only relay verb safe under RF. No
    // lockout check.
    hal::relay::set_bypass(on);
    return true;
}

bool home(Refusal &out_err) {
    if (refuse_if_rf(out_err)) return false;
    // Sim homing is instantaneous: anchor both axes at zero and mark
    // the controller homed. Real homing kicks off StallGuard hunts on
    // each axis and only sets homed_flag after both find their stop
    // (M1b.2 hardware integration).
    hal::motor::set_position(hal::Axis::L, 0);
    hal::motor::set_position(hal::Axis::C, 0);
    hal::encoder::set_count(hal::Axis::L, 0);
    hal::encoder::set_count(hal::Axis::C, 0);
    homed_flag = true;
    return true;
}

bool set_fwd_w_fake(float w, Refusal &out_err) {
    (void)out_err;
    hal::safety::inject_fwd_w(w);
    return true;
}

} // namespace app::motion
