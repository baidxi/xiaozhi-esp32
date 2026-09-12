// PCF85063 RTC driver for the xiaozhi MicroPython bridge.
//
// Onboard on esp32-s3-touch-lcd-4b (address 0x51, same firmware-owned I2C
// bus as the codec/IMU). Register map (all time fields BCD):
//   0x00 Control_1 (bit5 STOP), 0x01 Control_2, 0x03 RAM byte
//   0x04 Seconds (bit7 VL: oscillator stopped/invalid)
//   0x05 Minutes, 0x06 Hours, 0x07 Days, 0x08 Weekdays, 0x09 Months,
//   0x0A Years (00..99, base 2000)
//
// Python usage (via the xiaozhi module):
//   rtc = xiaozhi.rtc()
//   rtc.datetime()                            # read
//   rtc.datetime((2026, 9, 11, 4, 13, 5, 0))  # set (y,m,d,weekday,h,m,s)
//   rtc.valid()                               # False until first set

#include "py/mperrno.h"
#include "py/runtime.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "../xz_bridge.h"

#define TAG "xz_pcf85063"

#define PCF_ADDR_DEFAULT 0x51
#define PCF_REG_CONTROL1 0x00
#define PCF_REG_SECONDS 0x04
#define PCF_REG_YEARS 0x0A

static i2c_master_dev_handle_t s_rtc_dev = NULL;

int xz_mpy_pcf85063_register(const void* i2c_master_bus_handle, uint8_t addr) {
    if (s_rtc_dev != NULL) {
        return 0;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr != 0 ? addr : PCF_ADDR_DEFAULT,
        .scl_speed_hz = 400 * 1000,
    };
    esp_err_t err = i2c_master_bus_add_device((i2c_master_bus_handle_t)i2c_master_bus_handle,
                                              &dev_cfg, &s_rtc_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "PCF85063 ready at 0x%02x", dev_cfg.device_address);
    return 0;
}

static bool rtc_read(uint8_t reg, uint8_t* buf, size_t len) {
    return s_rtc_dev != NULL &&
           i2c_master_transmit_receive(s_rtc_dev, &reg, 1, buf, len, -1) == ESP_OK;
}

static bool rtc_write(uint8_t reg, const uint8_t* data, size_t len) {
    if (s_rtc_dev == NULL) {
        return false;
    }
    uint8_t buf[9] = {reg};
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_rtc_dev, buf, len + 1, -1) == ESP_OK;
}

static inline uint8_t bcd_decode(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }

static inline uint8_t bcd_encode(uint8_t val) { return ((val / 10) << 4) | (val % 10); }

/* ---- Python object ---- */

typedef struct _xz_rtc_obj_t {
    mp_obj_base_t base;
} xz_rtc_obj_t;

static void rtc_check_attached(void) {
    if (s_rtc_dev == NULL) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("no RTC on this board"));
    }
}

static mp_obj_t xz_rtc_make_new(const mp_obj_type_t* type, size_t n_args, size_t n_kw,
                                const mp_obj_t* args) {
    (void)n_args;
    (void)n_kw;
    (void)args;
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    rtc_check_attached();
    xz_rtc_obj_t* o = mp_obj_malloc(xz_rtc_obj_t, type);
    return MP_OBJ_FROM_PTR(o);
}

// datetime() -> (y, mo, d, wd, h, mi, s); datetime(tuple) -> set and return None.
static mp_obj_t xz_rtc_datetime(size_t n_args, const mp_obj_t* args) {
    rtc_check_attached();
    if (n_args == 1) {
        uint8_t raw[7];
        if (!rtc_read(PCF_REG_SECONDS, raw, sizeof(raw))) {
            mp_raise_OSError(MP_EIO);
        }
        mp_obj_t tuple[7] = {
            mp_obj_new_int(2000 + bcd_decode(raw[6])), mp_obj_new_int(bcd_decode(raw[5])),
            mp_obj_new_int(bcd_decode(raw[3])),        mp_obj_new_int(bcd_decode(raw[4])),
            mp_obj_new_int(bcd_decode(raw[2])),        mp_obj_new_int(bcd_decode(raw[1])),
            mp_obj_new_int(bcd_decode(raw[0] & 0x7F)),
        };
        return mp_obj_new_tuple(7, tuple);
    }

    mp_obj_t* seq = NULL;
    size_t seq_len = 0;
    mp_obj_get_array(args[1], &seq_len, &seq);
    if (seq_len != 7) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("expected (year, month, day, weekday, hour, minute, second)"));
    }
    int year = mp_obj_get_int(seq[0]) - 2000;
    uint8_t bcd[7] = {
        bcd_encode(mp_obj_get_int(seq[6])),  // seconds (VL cleared by writing)
        bcd_encode(mp_obj_get_int(seq[5])),  // minutes
        bcd_encode(mp_obj_get_int(seq[4])),  // hours
        bcd_encode(mp_obj_get_int(seq[2])),  // days
        bcd_encode(mp_obj_get_int(seq[3])),  // weekdays
        bcd_encode(mp_obj_get_int(seq[1])),  // months
        bcd_encode(year & 0xFF),             // years
    };
    // Stop the clock while updating, then restart (datasheet recommendation).
    rtc_write(PCF_REG_CONTROL1, &(uint8_t){0x20}, 1);
    bool ok = rtc_write(PCF_REG_SECONDS, bcd, sizeof(bcd));
    rtc_write(PCF_REG_CONTROL1, &(uint8_t){0x00}, 1);
    if (!ok) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(xz_rtc_datetime_obj, 1, 2, xz_rtc_datetime);

// False when the oscillator never ran since battery insert (time invalid).
static mp_obj_t xz_rtc_valid(mp_obj_t self_in) {
    (void)self_in;
    rtc_check_attached();
    uint8_t sec = 0;
    if (!rtc_read(PCF_REG_SECONDS, &sec, 1)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_obj_new_bool((sec & 0x80) == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_rtc_valid_obj, xz_rtc_valid);

static const mp_rom_map_elem_t xz_rtc_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_datetime), MP_ROM_PTR(&xz_rtc_datetime_obj)},
    {MP_ROM_QSTR(MP_QSTR_valid), MP_ROM_PTR(&xz_rtc_valid_obj)},
};
static MP_DEFINE_CONST_DICT(xz_rtc_locals_dict, xz_rtc_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(xz_rtc_type, MP_QSTR_RTC, MP_TYPE_FLAG_NONE, make_new,
                                xz_rtc_make_new, locals_dict, &xz_rtc_locals_dict);

// Exported accessor for the xiaozhi module (keeps the type file-local).
mp_obj_t xz_mpy_rtc_get(void) {
    rtc_check_attached();
    xz_rtc_obj_t* o = mp_obj_malloc(xz_rtc_obj_t, &xz_rtc_type);
    return MP_OBJ_FROM_PTR(o);
}
