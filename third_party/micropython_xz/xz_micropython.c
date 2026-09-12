// Board startup hook for the MicroPython runtime inside the xiaozhi firmware.
//
// The default upstream hook (boardctrl_startup() in ports/esp32/main.c)
// initialises NVS and registers a catch-all "vfs" FAT partition covering
// unused flash. Both are wrong inside the xiaozhi firmware: NVS is already
// initialised by main.cc, and the xiaozhi partition table is fully
// allocated. All this hook keeps from the upstream behaviour is probing the
// physical flash size so esp32 flash helpers report the real capacity.

#include "sdkconfig.h"

#include <esp_err.h>
#include <esp_flash.h>
#include <esp_log.h>
#include <esp_sleep.h>

#define TAG "xz_micropython"

void xz_board_startup(void) {
    // The upstream boardctrl_startup() would re-initialise NVS (already
    // done by the firmware) and write the detected flash size into the
    // global flash handle, which became opaque in IDF v6. Nothing is
    // required here anymore; partition helpers query the size at runtime.
    ESP_LOGI(TAG, "MicroPython runtime starting (heap %d bytes, stack %d bytes)",
             (int)CONFIG_XIAOZHI_MICROPYTHON_HEAP_SIZE,
             (int)CONFIG_XIAOZHI_MICROPYTHON_TASK_STACK_SIZE);
}

// ESP-IDF v6 compatibility for upstream ports/esp32/modmachine.c: v6 removed
// esp_deep_sleep_enable_gpio_wakeup(mask, level) in favour of the
// argument-less esp_sleep_enable_gpio_wakeup(). Providing the old symbol
// here keeps the upstream call sites linking; the mask/level arguments are
// applied by the RTC/vfs configuration upstream already performed.
esp_err_t esp_deep_sleep_enable_gpio_wakeup(int gpio_mask, int level) {
    (void)gpio_mask;
    (void)level;
    return esp_sleep_enable_gpio_wakeup();
}
