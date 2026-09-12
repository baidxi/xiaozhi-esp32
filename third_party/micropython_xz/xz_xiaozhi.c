// The `xiaozhi` MicroPython module: board status + registered peripherals.
//
// Design: boards and main.cc register capabilities through the C API in
// xz_bridge.h during startup. The registry lives in C globals (surviving
// MicroPython soft resets); Python objects are cheap wrappers re-created
// by each call. Unregistered capabilities raise NotImplementedError, which
// keeps scripts portable across boards.
//
//   import xiaozhi
//   xiaozhi.board_type()            # 'waveshare-esp32-s3-touch-lcd-4b'
//   xiaozhi.temperature()           # chip temperature or None
//   xiaozhi.battery()               # {'level': 82, 'charging': False, ...}
//   imu = xiaozhi.imu(); imu.accel()
//   servo = xiaozhi.servo(); servo.set_angle(0, 90.0)
//   s = xiaozhi.screen(); s.clear('black'); s.text(10, 10, 'hi', 'white')
//   wifi = xiaozhi.wifi(); wifi.ssid('ap'); wifi.connect()   # 'OK'/'FALSE'

#include "py/runtime.h"

#include "xz_bridge.h"

/* ---- C registry (populated at startup, survives soft resets) ---- */

static const xz_mpy_board_api_t* s_board_api = NULL;
static xz_mpy_servo_api_t s_servo_api = {0};

void xz_mpy_set_board_api(const xz_mpy_board_api_t* api) { s_board_api = api; }

void xz_mpy_servo_register(const xz_mpy_servo_api_t* api) {
    if (api != NULL) {
        s_servo_api = *api;
    }
}

/* Provided by the drivers directory / xz_display.c. */
extern mp_obj_t xz_mpy_imu_get(void);
extern mp_obj_t xz_mpy_rtc_get(void);
extern mp_obj_t xz_mpy_display_get(void);
extern mp_obj_t xz_mpy_wifi_get(void);
extern mp_obj_t xz_mpy_log_get(void);

/* Registry entries implemented here (declared in xz_bridge.h). */
static const xz_mpy_display_api_t* s_display_api = NULL;

void xz_mpy_display_register(const xz_mpy_display_api_t* api) {
    if (api != NULL) {
        s_display_api = api;
    }
}

const xz_mpy_display_api_t* xz_mpy_display_api(void) { return s_display_api; }

static const xz_mpy_wifi_api_t* s_wifi_api = NULL;

void xz_mpy_wifi_register(const xz_mpy_wifi_api_t* api) {
    if (api != NULL) {
        s_wifi_api = api;
    }
}

const xz_mpy_wifi_api_t* xz_mpy_wifi_api(void) { return s_wifi_api; }

/* ---- Board status ---- */

static mp_obj_t xz_board_type(void) {
    if (s_board_api == NULL || s_board_api->get_board_type == NULL) {
        return mp_const_none;
    }
    char buf[64];
    s_board_api->get_board_type(buf, sizeof(buf));
    return mp_obj_new_str(buf, strlen(buf));
}
static MP_DEFINE_CONST_FUN_OBJ_0(xz_board_type_obj, xz_board_type);

static mp_obj_t xz_board_temperature(void) {
    if (s_board_api == NULL || s_board_api->get_temperature == NULL) {
        return mp_const_none;
    }
    float t = 0.0f;
    if (!s_board_api->get_temperature(&t)) {
        return mp_const_none;
    }
    return mp_obj_new_float(t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(xz_board_temperature_obj, xz_board_temperature);

static mp_obj_t xz_board_battery(void) {
    if (s_board_api == NULL || s_board_api->get_battery == NULL) {
        return mp_const_none;
    }
    int level = 0;
    bool charging = false, discharging = false;
    if (!s_board_api->get_battery(&level, &charging, &discharging)) {
        return mp_const_none;
    }
    mp_obj_t dict = mp_obj_new_dict(3);
    mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(MP_QSTR_level), mp_obj_new_int(level));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(MP_QSTR_charging),
                      mp_obj_new_bool(charging));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(MP_QSTR_discharging),
                      mp_obj_new_bool(discharging));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(xz_board_battery_obj, xz_board_battery);

/* ---- Peripheral accessors (functions: soft-reset safe) ---- */

static mp_obj_t xz_get_imu(void) {
    return xz_mpy_imu_get();  // raises when the IMU is not attached
}
static MP_DEFINE_CONST_FUN_OBJ_0(xz_get_imu_obj, xz_get_imu);

/* ---- Servo wrapper over the registered callback table ---- */

typedef struct _xz_servo_obj_t {
    mp_obj_base_t base;
} xz_servo_obj_t;

static void servo_check_registered(void) {
    if (s_servo_api.set_angle == NULL) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("no servo on this board"));
    }
}

static mp_obj_t xz_servo_make_new(const mp_obj_type_t* type, size_t n_args, size_t n_kw,
                                  const mp_obj_t* args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    servo_check_registered();
    xz_servo_obj_t* o = mp_obj_malloc(xz_servo_obj_t, type);
    return MP_OBJ_FROM_PTR(o);
}

static mp_obj_t xz_servo_set_angle(mp_obj_t self_in, mp_obj_t channel_in, mp_obj_t angle_in) {
    (void)self_in;
    servo_check_registered();
    mp_int_t ch = mp_obj_get_int(channel_in);
    if (ch < 0 || ch > s_servo_api.channel_max) {
        mp_raise_ValueError(MP_ERROR_TEXT("servo channel out of range"));
    }
    mp_float_t angle = mp_obj_get_float(angle_in);
    int rc = s_servo_api.set_angle((int)ch, (float)angle);
    if (rc != 0) {
        mp_raise_OSError(rc);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(xz_servo_set_angle_obj, xz_servo_set_angle);

static mp_obj_t xz_servo_enable(mp_obj_t self_in, mp_obj_t channel_in, mp_obj_t enabled_in) {
    (void)self_in;
    servo_check_registered();
    mp_int_t ch = mp_obj_get_int(channel_in);
    if (ch < 0 || ch > s_servo_api.channel_max) {
        mp_raise_ValueError(MP_ERROR_TEXT("servo channel out of range"));
    }
    int rc = s_servo_api.set_enable((int)ch, mp_obj_is_true(enabled_in) ? 1 : 0);
    if (rc != 0) {
        mp_raise_OSError(rc);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(xz_servo_enable_obj, xz_servo_enable);

static mp_obj_t xz_servo_center(mp_obj_t self_in) {
    (void)self_in;
    servo_check_registered();
    int rc = s_servo_api.center_all();
    if (rc != 0) {
        mp_raise_OSError(rc);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_servo_center_obj, xz_servo_center);

static mp_obj_t xz_servo_channels(mp_obj_t self_in) {
    (void)self_in;
    servo_check_registered();
    return mp_obj_new_int(s_servo_api.channel_max + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_servo_channels_obj, xz_servo_channels);

static const mp_rom_map_elem_t xz_servo_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_set_angle), MP_ROM_PTR(&xz_servo_set_angle_obj)},
    {MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&xz_servo_enable_obj)},
    {MP_ROM_QSTR(MP_QSTR_center), MP_ROM_PTR(&xz_servo_center_obj)},
    {MP_ROM_QSTR(MP_QSTR_channels), MP_ROM_PTR(&xz_servo_channels_obj)},
};
static MP_DEFINE_CONST_DICT(xz_servo_locals_dict, xz_servo_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(xz_servo_type, MP_QSTR_Servo, MP_TYPE_FLAG_NONE, make_new,
                                xz_servo_make_new, locals_dict, &xz_servo_locals_dict);

static mp_obj_t xz_get_servo(void) {
    servo_check_registered();
    xz_servo_obj_t* o = mp_obj_malloc(xz_servo_obj_t, &xz_servo_type);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_0(xz_get_servo_obj, xz_get_servo);

static mp_obj_t xz_get_rtc(void) {
    return xz_mpy_rtc_get();  // raises when the RTC is not attached
}
static MP_DEFINE_CONST_FUN_OBJ_0(xz_get_rtc_obj, xz_get_rtc);

static mp_obj_t xz_get_screen(void) {
    return xz_mpy_display_get();  // raises when no display callbacks registered
}
static MP_DEFINE_CONST_FUN_OBJ_0(xz_get_screen_obj, xz_get_screen);

static mp_obj_t xz_get_wifi(void) {
    return xz_mpy_wifi_get();  // raises when no wifi callbacks registered
}
static MP_DEFINE_CONST_FUN_OBJ_0(xz_get_wifi_obj, xz_get_wifi);

static mp_obj_t xz_get_log(void) { return xz_mpy_log_get(); }
static MP_DEFINE_CONST_FUN_OBJ_0(xz_get_log_obj, xz_get_log);

/* ---- Module ---- */

static const mp_rom_map_elem_t xz_xiaozhi_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_xiaozhi)},
    {MP_ROM_QSTR(MP_QSTR_board_type), MP_ROM_PTR(&xz_board_type_obj)},
    {MP_ROM_QSTR(MP_QSTR_temperature), MP_ROM_PTR(&xz_board_temperature_obj)},
    {MP_ROM_QSTR(MP_QSTR_battery), MP_ROM_PTR(&xz_board_battery_obj)},
    {MP_ROM_QSTR(MP_QSTR_imu), MP_ROM_PTR(&xz_get_imu_obj)},
    {MP_ROM_QSTR(MP_QSTR_servo), MP_ROM_PTR(&xz_get_servo_obj)},
    {MP_ROM_QSTR(MP_QSTR_rtc), MP_ROM_PTR(&xz_get_rtc_obj)},
    {MP_ROM_QSTR(MP_QSTR_screen), MP_ROM_PTR(&xz_get_screen_obj)},
    {MP_ROM_QSTR(MP_QSTR_wifi), MP_ROM_PTR(&xz_get_wifi_obj)},
    {MP_ROM_QSTR(MP_QSTR_log), MP_ROM_PTR(&xz_get_log_obj)},
};
static MP_DEFINE_CONST_DICT(xz_xiaozhi_globals, xz_xiaozhi_globals_table);

const mp_obj_module_t xz_xiaozhi_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t*)&xz_xiaozhi_globals,
};

MP_REGISTER_MODULE(MP_QSTR_xiaozhi, xz_xiaozhi_module);
