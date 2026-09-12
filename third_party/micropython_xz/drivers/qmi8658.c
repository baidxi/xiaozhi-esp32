// QMI8658A six-axis IMU driver for the xiaozhi MicroPython bridge.
//
// The chip is attached to a firmware-owned I2C bus (e.g. the audio codec
// bus on esp32-s3-touch-lcd-4b, address 0x6B). The I2C device handle is a
// C global so it survives MicroPython soft resets; the ESP-IDF I2C master
// driver serialises transactions on the bus, which makes concurrent access
// from the mp_task safe next to the codec/touch traffic.
//
// Register map (QMI8658A datasheet):
//   0x00 WHO_AM_I (0x05)   0x02 CTRL1 (0x40: address auto-increment)
//   0x03 CTRL2 accel cfg    0x04 CTRL3 gyro cfg
//   0x08 CTRL7 (aEN|gEN)    0x2E STATUS0 (data-ready bits)
//
// Data window verified on hardware (esp32-s3-touch-lcd-4b): the live
// samples live at 0x34..0x47 -- 0x34 temperature (1/256 degC, slow drift),
// then 12 data bytes from 0x36. The first int16 at 0x36 reads +1g flat and
// -1g flipped (silicon-down), i.e. it is the gravity axis on this mount;
// axis order follows the chip mount and is reported as-is.
//
// Scaling follows the Waveshare demo for this board:
//   CTRL2 = 0x23 -> +/-2g   (16384 LSB per g)
//   CTRL3 = 0x53 -> +/-256dps (128 LSB per dps)

#include "py/mperrno.h"
#include "py/runtime.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../xz_bridge.h"

#define TAG "xz_qmi8658"

#define QMI_REG_WHOAMI 0x00
#define QMI_REG_CTRL1 0x02
#define QMI_REG_CTRL2 0x03
#define QMI_REG_CTRL3 0x04
#define QMI_REG_CTRL7 0x08
#define QMI_REG_DATA_BASE 0x36
#define QMI_REG_TEMP_BASE 0x34

#define QMI_WHOAMI_VALUE 0x05
#define QMI_ACCEL_LSB_PER_G 16384.0f
#define QMI_GYRO_LSB_PER_DPS 128.0f

static i2c_master_dev_handle_t s_qmi_dev = NULL;

int xz_mpy_qmi8658_register(const void* i2c_master_bus_handle, uint8_t addr) {
    if (s_qmi_dev != NULL) {
        return 0;  // already attached
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400 * 1000,
    };
    esp_err_t err = i2c_master_bus_add_device((i2c_master_bus_handle_t)i2c_master_bus_handle,
                                              &dev_cfg, &s_qmi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_add_device failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint8_t who = 0;
    err = i2c_master_transmit_receive(s_qmi_dev, &(uint8_t){QMI_REG_WHOAMI}, 1, &who, 1, -1);
    if (err != ESP_OK || who != QMI_WHOAMI_VALUE) {
        ESP_LOGE(TAG, "QMI8658 not found (whoami=0x%02x err=%s)", who, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_qmi_dev);
        s_qmi_dev = NULL;
        return -1;
    }

    uint8_t init[][2] = {
        {QMI_REG_CTRL1, 0x40},  // address auto-increment
        {QMI_REG_CTRL2, 0x23},  // accel +/-2g
        {QMI_REG_CTRL3, 0x53},  // gyro +/-256dps
        {QMI_REG_CTRL7, 0x03},  // accel + gyro enable
    };
    for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); i++) {
        err = i2c_master_transmit(s_qmi_dev, init[i], 2, -1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init reg 0x%02x failed: %s", init[i][0], esp_err_to_name(err));
            i2c_master_bus_rm_device(s_qmi_dev);
            s_qmi_dev = NULL;
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        // Read back and verify so a silent configuration failure shows up in
        // the boot log instead of as frozen sensor data later.
        uint8_t rb = 0;
        if (i2c_master_transmit_receive(s_qmi_dev, &init[i][0], 1, &rb, 1, -1) != ESP_OK ||
            rb != init[i][1]) {
            ESP_LOGE(TAG, "cfg verify failed: reg 0x%02x wrote 0x%02x read 0x%02x", init[i][0],
                     init[i][1], rb);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(10));  // settle before the first sample
    ESP_LOGI(TAG, "QMI8658A ready at 0x%02x", addr);
    return 0;
}

/* ---- Python object ---- */

typedef struct _xz_imu_obj_t {
    mp_obj_base_t base;
} xz_imu_obj_t;

static void imu_check_attached(void) {
    if (s_qmi_dev == NULL) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("no IMU on this board"));
    }
}

static int imu_read_block(uint8_t reg, uint8_t* buf, size_t len) {
    return i2c_master_transmit_receive(s_qmi_dev, &reg, 1, buf, len, -1) == ESP_OK ? 0 : -1;
}

static mp_obj_t xz_imu_make_new(const mp_obj_type_t* type, size_t n_args, size_t n_kw,
                                const mp_obj_t* args) {
    (void)n_args;
    (void)n_kw;
    (void)args;
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    imu_check_attached();
    xz_imu_obj_t* o = mp_obj_malloc(xz_imu_obj_t, type);
    return MP_OBJ_FROM_PTR(o);
}

static mp_obj_t xz_imu_accel(mp_obj_t self_in) {
    (void)self_in;
    imu_check_attached();
    uint8_t raw[6];
    if (imu_read_block(QMI_REG_DATA_BASE, raw, sizeof(raw)) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    int16_t x = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t y = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t z = (int16_t)((raw[5] << 8) | raw[4]);
    mp_obj_t tuple[3] = {
        mp_obj_new_float(x / QMI_ACCEL_LSB_PER_G),
        mp_obj_new_float(y / QMI_ACCEL_LSB_PER_G),
        mp_obj_new_float(z / QMI_ACCEL_LSB_PER_G),
    };
    return mp_obj_new_tuple(3, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_imu_accel_obj, xz_imu_accel);

static mp_obj_t xz_imu_gyro(mp_obj_t self_in) {
    (void)self_in;
    imu_check_attached();
    uint8_t raw[6];
    if (imu_read_block(QMI_REG_DATA_BASE + 6, raw, sizeof(raw)) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    int16_t x = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t y = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t z = (int16_t)((raw[5] << 8) | raw[4]);
    mp_obj_t tuple[3] = {
        mp_obj_new_float(x / QMI_GYRO_LSB_PER_DPS),
        mp_obj_new_float(y / QMI_GYRO_LSB_PER_DPS),
        mp_obj_new_float(z / QMI_GYRO_LSB_PER_DPS),
    };
    return mp_obj_new_tuple(3, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_imu_gyro_obj, xz_imu_gyro);

static mp_obj_t xz_imu_temperature(mp_obj_t self_in) {
    (void)self_in;
    imu_check_attached();
    uint8_t raw[2];
    if (imu_read_block(QMI_REG_TEMP_BASE, raw, sizeof(raw)) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    int16_t t = (int16_t)((raw[1] << 8) | raw[0]);
    return mp_obj_new_float(t / 256.0f);
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_imu_temperature_obj, xz_imu_temperature);

bool xz_mpy_qmi8658_read_temperature(float* out_celsius) {
    if (s_qmi_dev == NULL) {
        return false;
    }
    uint8_t raw[2];
    if (imu_read_block(QMI_REG_TEMP_BASE, raw, sizeof(raw)) != 0) {
        return false;
    }
    int16_t t = (int16_t)((raw[1] << 8) | raw[0]);
    if (t == INT16_MIN) {
        return false;  // 0x8000: sensor not sampling yet, temperature invalid
    }
    *out_celsius = t / 256.0f;
    return true;
}

// Diagnostic: return registers 0x00..0x5F as bytes. Useful to verify the
// CTRL writes landed and to locate the live data window.
static mp_obj_t xz_imu_dump(mp_obj_t self_in) {
    (void)self_in;
    imu_check_attached();
    uint8_t buf[0x60];
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (imu_read_block((uint8_t)i, &buf[i], 1) != 0) {
            mp_raise_OSError(MP_EIO);
        }
    }
    return mp_obj_new_bytes(buf, sizeof(buf));
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_imu_dump_obj, xz_imu_dump);

// Low-level probe: read(reg, len) / write(reg, value). Lets the live chip
// reveal its data window and sync requirements from the REPL.
static mp_obj_t xz_imu_read(mp_obj_t self_in, mp_obj_t reg_in, mp_obj_t len_in) {
    (void)self_in;
    imu_check_attached();
    uint8_t reg = mp_obj_get_int(reg_in);
    uint8_t len = mp_obj_get_int(len_in);
    if (len > 64) {
        mp_raise_ValueError(MP_ERROR_TEXT("len must be <= 64"));
    }
    uint8_t buf[64];
    if (imu_read_block(reg, buf, len) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_obj_new_bytes(buf, len);
}
static MP_DEFINE_CONST_FUN_OBJ_3(xz_imu_read_obj, xz_imu_read);

static mp_obj_t xz_imu_write(mp_obj_t self_in, mp_obj_t reg_in, mp_obj_t value_in) {
    (void)self_in;
    imu_check_attached();
    uint8_t data[2] = {(uint8_t)mp_obj_get_int(reg_in), (uint8_t)mp_obj_get_int(value_in)};
    if (s_qmi_dev == NULL || i2c_master_transmit(s_qmi_dev, data, sizeof(data), -1) != ESP_OK) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(xz_imu_write_obj, xz_imu_write);

static mp_obj_t xz_imu_whoami(mp_obj_t self_in) {
    (void)self_in;
    imu_check_attached();
    uint8_t who = 0;
    if (imu_read_block(QMI_REG_WHOAMI, &who, 1) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_obj_new_int(who);
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_imu_whoami_obj, xz_imu_whoami);

static const mp_rom_map_elem_t xz_imu_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_accel), MP_ROM_PTR(&xz_imu_accel_obj)},
    {MP_ROM_QSTR(MP_QSTR_gyro), MP_ROM_PTR(&xz_imu_gyro_obj)},
    {MP_ROM_QSTR(MP_QSTR_temperature), MP_ROM_PTR(&xz_imu_temperature_obj)},
    {MP_ROM_QSTR(MP_QSTR_whoami), MP_ROM_PTR(&xz_imu_whoami_obj)},
    {MP_ROM_QSTR(MP_QSTR_dump), MP_ROM_PTR(&xz_imu_dump_obj)},
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&xz_imu_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&xz_imu_write_obj)},
};
static MP_DEFINE_CONST_DICT(xz_imu_locals_dict, xz_imu_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(xz_imu_type, MP_QSTR_IMU, MP_TYPE_FLAG_NONE, make_new,
                                xz_imu_make_new, locals_dict, &xz_imu_locals_dict);

// Exported accessor for the xiaozhi module (keeps the type file-local).
mp_obj_t xz_mpy_imu_get(void) {
    imu_check_attached();
    xz_imu_obj_t* o = mp_obj_malloc(xz_imu_obj_t, &xz_imu_type);
    return MP_OBJ_FROM_PTR(o);
}
