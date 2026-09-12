#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "application.h"
#include "board.h"
#if CONFIG_XIAOZHI_MICROPYTHON
#include "xz_bridge.h"
#include "xz_micropython.h"
#endif

#define TAG "main"

extern "C" void app_main(void) {
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
#if CONFIG_XIAOZHI_MICROPYTHON
    // Feed board status through the bridge, then boot the embedded
    // MicroPython runtime (serial REPL) alongside the assistant.
    static const xz_mpy_board_api_t board_api = {
        .get_board_type =
            [](char* buf, size_t n) {
                auto type = Board::GetInstance().GetBoardType();
                snprintf(buf, n, "%s", type.c_str());
            },
        .get_temperature =
            [](float* t) {
                if (Board::GetInstance().GetTemperature(*t)) {
                    return true;
                }
                // Boards without a tsen implementation: fall back to the onboard
                // IMU die temperature registered through the bridge.
                return xz_mpy_qmi8658_read_temperature(t);
            },
        .get_battery =
            [](int* level, bool* charging, bool* discharging) {
                return Board::GetInstance().GetBatteryLevel(*level, *charging, *discharging);
            },
    };
    xz_mpy_set_board_api(&board_api);
    extern void xz_micropython_register_display(void);
    xz_micropython_register_display();
    extern void xz_micropython_log_init(void);
    xz_micropython_log_init();
    xz_micropython_boot();
#endif
    app.Run();  // This function runs the main event loop and never returns
}
