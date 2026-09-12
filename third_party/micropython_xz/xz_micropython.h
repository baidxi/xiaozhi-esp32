#ifndef XZ_MICROPYTHON_H
#define XZ_MICROPYTHON_H

#ifdef __cplusplus
extern "C" {
#endif

// Boot the embedded MicroPython runtime.
//
// This creates the "mp_task" FreeRTOS task that runs the serial REPL (and
// boot.py/main.py once a python filesystem partition exists). The task runs
// independently of the xiaozhi application loop; call this once, after
// Application::Initialize(), alongside the assistant startup.
//
// Implemented by upstream ports/esp32/main.c through the
// MICROPY_ESP_IDF_ENTRY macro override in boards/xiaozhi/mpconfigboard.h.
void xz_micropython_boot(void);

#ifdef __cplusplus
}
#endif

#endif  // XZ_MICROPYTHON_H
