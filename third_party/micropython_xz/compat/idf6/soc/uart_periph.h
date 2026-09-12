// ESP-IDF v6 compatibility shim for upstream MicroPython.
//
// IDF v6 removed soc/<periph>_periph.h and moved the peripheral maps to
// esp_hal_* components as hal/<periph>_periph.h. Upstream
// ports/esp32/uart.c still includes the old path.

#pragma once
#include "hal/uart_periph.h"
