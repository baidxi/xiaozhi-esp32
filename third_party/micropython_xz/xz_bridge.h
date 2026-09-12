#ifndef XZ_BRIDGE_H
#define XZ_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Xiaozhi <-> MicroPython bridge API (C, header-only dependencies).
 *
 * Boards and the main component register hardware capabilities here during
 * startup; the `xiaozhi` Python module exposes them to scripts. The registry
 * lives in C globals so it survives MicroPython soft resets; Python-side
 * objects are re-created on demand.
 *
 * All functions are no-ops (return false / -1) when the MicroPython runtime
 * is disabled or the capability was never registered.
 */

/* ---- Board status callbacks (implemented by the main component) ---- */

typedef struct {
    /* Return the board type string (Board::GetBoardType). */
    void (*get_board_type)(char* buf, size_t buf_size);
    /* Board::GetTemperature; return false when unsupported. */
    bool (*get_temperature)(float* out_celsius);
    /* Board::GetBatteryLevel; return false when unsupported. */
    bool (*get_battery)(int* level_pct, bool* charging, bool* discharging);
} xz_mpy_board_api_t;

/* Called once by main.cc before the MicroPython runtime boots. */
void xz_mpy_set_board_api(const xz_mpy_board_api_t* api);

/* ---- Servo (callback style: boards keep owning the controller) ---- */

typedef struct {
    /* Set one channel angle (degrees, float). Return 0 on success. */
    int (*set_angle)(int channel, float angle_deg);
    /* Enable (1) / free (0) one channel's torque. Return 0 on success. */
    int (*set_enable)(int channel, int enabled);
    /* Centre every channel to 90 degrees. Return 0 on success. */
    int (*center_all)(void);
    /* Upper bound of valid channel indices (e.g. 11 -> channels 0..11). */
    int channel_max;
} xz_mpy_servo_api_t;

/* Register the board's servo controller. Safe to call once at startup. */
void xz_mpy_servo_register(const xz_mpy_servo_api_t* api);

/* ---- QMI8658 six-axis IMU (driver lives inside the component) ---- */

/*
 * Attach the onboard QMI8658A (address 0x6B) to the given I2C bus. The bus
 * must be created and owned by the firmware (e.g. the audio codec bus); the
 * driver only adds an I2C device to it. Returns 0 on success.
 */
int xz_mpy_qmi8658_register(const void* i2c_master_bus_handle, uint8_t addr);

/*
 * Attach the onboard PCF85063 RTC (address 0x51) to the given I2C bus.
 * Returns 0 on success.
 */
int xz_mpy_pcf85063_register(const void* i2c_master_bus_handle, uint8_t addr);

/*
 * Read the QMI8658 die temperature (degC). Returns false when the IMU was
 * never registered or the read failed. Used as the board-temperature
 * fallback (boards without an esp32 tsen implementation).
 */
bool xz_mpy_qmi8658_read_temperature(float* out_celsius);

/*
 * Drawing surface on the board screen. Implemented by the main component on
 * top of an LVGL overlay canvas (the firmware owns the display); colours
 * are 0x00RRGGBB. All callbacks run on the caller (mp_task) context and
 * must internally serialise with the LVGL task.
 */
typedef struct {
    void (*clear)(uint32_t color);
    void (*pixel)(int x, int y, uint32_t color);
    void (*line)(int x1, int y1, int x2, int y2, uint32_t color);
    void (*rect)(int x, int y, int w, int h, uint32_t color, bool fill);
    void (*circle)(int cx, int cy, int r, uint32_t color, bool fill);
    void (*text)(int x, int y, const char* utf8, uint32_t color);
    void (*size)(int* out_w, int* out_h);
    /* Hide the overlay and restore the assistant UI. */
    void (*hide)(void);
} xz_mpy_display_api_t;

/* Register the drawing callbacks (once, at startup). */
void xz_mpy_display_register(const xz_mpy_display_api_t* api);

/* Returns NULL until registered. */
const xz_mpy_display_api_t* xz_mpy_display_api(void);

/* ---- WiFi station configuration (registered by the WifiBoard layer) ---- */

/* One scanned access point (see xz_mpy_wifi_api_t.scan). */
typedef struct {
    char ssid[33];
    bool secure;  // any auth mode other than open
    uint8_t channel;
    int8_t rssi;  // dBm (negative)
} xz_mpy_wifi_ap_t;

/*
 * The implementation follows the firmware's own provisioning path
 * (SsidManager + WifiManager.StartStation, same as blufi and the config
 * portal): connect() persists the credentials on success and rolls the
 * temporary entry back on failure, so the assistant's current network is
 * unaffected. connect() blocks for up to timeout_ms.
 */
typedef struct {
    /* Connect with the given credentials. Return 0 on success. */
    int (*connect)(const char* ssid, const char* password, int timeout_ms);
    /* Persist credentials for the assistant (used after reboot). 0 on success. */
    int (*save)(const char* ssid, const char* password);
    /*
     * Scan nearby APs and fill out_aps (deduplicated, RSSI order, hidden
     * APs skipped) up to max_aps entries. Returns the network count, or -1
     * on failure.
     */
    int (*scan)(xz_mpy_wifi_ap_t* out_aps, int max_aps);
    /* Link status queries; optional (NULL) fields report disconnected/empty. */
    bool (*is_connected)(void);
    void (*get_ssid)(char* buf, size_t buf_size);
    void (*get_ip)(char* buf, size_t buf_size);
    int (*get_rssi)(void);
} xz_mpy_wifi_api_t;

/* Register the board's WiFi callbacks (once, at startup). */
void xz_mpy_wifi_register(const xz_mpy_wifi_api_t* api);

/* Returns NULL until registered. */
const xz_mpy_wifi_api_t* xz_mpy_wifi_api(void);

#ifdef __cplusplus
}
#endif

#endif /* XZ_BRIDGE_H */
