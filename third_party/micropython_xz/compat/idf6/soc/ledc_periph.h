// ESP-IDF v6 compatibility shim for upstream MicroPython.
//
// IDF v6 removed soc/<periph>_periph.h and moved the peripheral maps to
// esp_hal_* components as hal/<periph>_periph.h. Upstream
// ports/esp32/machine_pwm.c still includes the old path and accesses the
// v5 member layout:
//
//     ledc_periph_signal[mode].sig_out0_idx + channel
//
// The v6 layout nests the per-channel signal indexes:
//
//     ledc_periph_signal[group].speed_mode[mode].sig_out_idx[channel]
//
// sig_out0_idx (the channel-0 output signal) therefore maps to
// speed_mode[0].sig_out_idx[0]; adding the channel index is identical in
// both layouts. The macro below rewrites the member access accordingly.
// It is only referenced by upstream machine_pwm.c.

#pragma once
#include "hal/ledc_periph.h"

#define sig_out0_idx speed_mode[0].sig_out_idx[0]
