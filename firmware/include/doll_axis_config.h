// Doll axis controller (src/doll_axis): pins and mechanics.
// One NEMA17 stepper moves the doll front/back on a single rail, one optical sensor gives home.
//
// EVERY VALUE HERE IS A PLACEHOLDER (TODO) until the user decides the board, the stepper driver,
// the wiring and the drive train (belt/pulley or lead screw). Nothing was tested on hardware.
// After the parts are fixed, calibrate as described in firmware/README.md ("인형 1축 제어기").
#pragma once

#include <stdint.h>

#include <hm_axis.h>
#include <hm_axis_protocol.h>

namespace doll_axis {

const char kFirmwareName[] = "doll_axis";

// ---------------------------------------------------------------- pins (TODO: wiring not decided)
// Logic is 3.3 V on both boards: the sensor output and the driver inputs must be 3.3 V signals
// (Teensy 4.1 pins are NOT 5 V tolerant). The endstop input uses the internal pull-up.
#if defined(HM_BOARD_TEENSY41_AXIS)
const uint8_t kPinStep = 2;     // TODO
const uint8_t kPinDir = 3;      // TODO
const uint8_t kPinEnable = 4;   // TODO
const uint8_t kPinEndstop = 5;  // TODO
#elif defined(HM_BOARD_ESP32_AXIS)
// ESP32 DevKit: avoids the boot-strapping pins 0/2/5/12/15, the flash pins 6-11, UART0 1/3 and
// the input-only pins 34-39 (no internal pull-up).
const uint8_t kPinStep = 25;     // TODO
const uint8_t kPinDir = 26;      // TODO
const uint8_t kPinEnable = 27;   // TODO
const uint8_t kPinEndstop = 33;  // TODO
#else
#error "doll_axis: build with -DHM_BOARD_TEENSY41_AXIS=1 or -DHM_BOARD_ESP32_AXIS=1"
#endif

// ---------------------------------------------------------------- mechanics and motion
// steps_per_mm = motor steps/rev x microsteps / travel per rev (mm). TODO: drive train not bought.
//   GT2 belt (2 mm pitch) + 20-tooth pulley, 200 steps/rev, 1/16 microstep: 200*16/40   = 80
//   T8 lead screw (8 mm lead),               200 steps/rev, 1/16 microstep: 200*16/8    = 400
const hm::axis::AxisConfig kAxisConfig = {
    80.0f,    // steps_per_mm              TODO (placeholder: GT2 20T, 1/16)
    60.0f,    // max_speed_mm_s            TODO tune on the model
    300.0f,   // accel_mm_s2               TODO tune on the model
    300.0f,   // travel_mm                 TODO measure the usable rail length
    -1,       // home_dir                  TODO -1: sensor at the 0 end, +1: at the far end
    false,    // invert_dir                TODO flip if HOME runs away from the sensor
    true,     // endstop_active_high       TODO check with STATUS? (endstop=1 when blocked)
    true,     // enable_active_low         A4988 / DRV8825 / TMC2209: EN low = driver on
    20.0f,    // home_fast_mm_s
    3.0f,     // home_slow_mm_s
    5.0f,     // home_backoff_mm
    20.0f,    // home_margin_mm            first seek gives up after travel + margin
    8.0f,     // endstop_zone_mm           >= backoff
    200,      // endstop_debounce_us
    60000,    // idle_disable_ms           TODO 0 = always hold
    false,    // unhome_on_idle_disable    TODO true if the carriage moves freely when unpowered
    5,        // step_pulse_us             DRV8825 needs >= 1.9 us, A4988 >= 1 us
};

// Named positions in mm from the 0 end, RAM defaults (SET changes them until reset).
// TODO: placeholders, calibrate on the model and copy the final values here.
const hm::axis::NamedPosition kNamedPositions[] = {
    {"entrance", 10.0f},
    {"bed", 120.0f},
    {"bathroom", 220.0f},
};
const uint8_t kNamedPositionCount =
    static_cast<uint8_t>(sizeof(kNamedPositions) / sizeof(kNamedPositions[0]));

}  // namespace doll_axis
