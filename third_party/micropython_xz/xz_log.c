// `xiaozhi.log` — capture and control the firmware log from Python.
//
//   import xiaozhi
//   xiaozhi.log().quiet(True)     # silence the console (clean REPL)
//   xiaozhi.log().read()          # -> str, new log lines (consumed)
//   xiaozhi.log().clear()         # drop buffered lines
//   xiaozhi.log().level('warn')   # global level: none/error/warn/info/debug
//
// Buffer capture always runs; quiet() only stops console printing, so
// debugging output stays queryable while the graphical-programming REPL
// stays clean.

#include "py/runtime.h"

#include "xz_bridge.h"

/* Implemented in main/micropython_logging.cc. */
int xz_micropython_log_read(char* buf, int buf_size);
void xz_micropython_log_clear(void);
void xz_micropython_log_quiet(int enable);

typedef struct _xz_log_obj_t {
    mp_obj_base_t base;
} xz_log_obj_t;

static mp_obj_t xz_log_make_new(const mp_obj_type_t* type, size_t n_args, size_t n_kw,
                                const mp_obj_t* args) {
    (void)n_args;
    (void)n_kw;
    (void)args;
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    xz_log_obj_t* o = mp_obj_malloc(xz_log_obj_t, type);
    return MP_OBJ_FROM_PTR(o);
}

static mp_obj_t xz_log_read(mp_obj_t self_in) {
    (void)self_in;
    char buf[1024];
    int n = xz_micropython_log_read(buf, sizeof(buf));
    return mp_obj_new_str(buf, n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_log_read_obj, xz_log_read);

static mp_obj_t xz_log_clear(mp_obj_t self_in) {
    (void)self_in;
    xz_micropython_log_clear();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_log_clear_obj, xz_log_clear);

static mp_obj_t xz_log_quiet(mp_obj_t self_in, mp_obj_t enable_in) {
    (void)self_in;
    xz_micropython_log_quiet(mp_obj_is_true(enable_in) ? 1 : 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(xz_log_quiet_obj, xz_log_quiet);

static mp_obj_t xz_log_level(mp_obj_t self_in, mp_obj_t level_in) {
    (void)self_in;
    // Map a level string onto esp_log_level_t via the bridge-free path:
    // the component cannot include esp_log.h, so we forward to the main
    // implementation through the same C symbols used elsewhere.
    const char* s = mp_obj_str_get_str(level_in);
    int lvl;
    if (strcmp(s, "none") == 0) {
        lvl = 0;
    } else if (strcmp(s, "error") == 0) {
        lvl = 1;
    } else if (strcmp(s, "warn") == 0 || strcmp(s, "warning") == 0) {
        lvl = 2;
    } else if (strcmp(s, "info") == 0) {
        lvl = 3;
    } else if (strcmp(s, "debug") == 0) {
        lvl = 4;
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("level must be none/error/warn/info/debug"));
    }
    extern void xz_micropython_log_level(int level);
    xz_micropython_log_level(lvl);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(xz_log_level_obj, xz_log_level);

static const mp_rom_map_elem_t xz_log_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&xz_log_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&xz_log_clear_obj)},
    {MP_ROM_QSTR(MP_QSTR_quiet), MP_ROM_PTR(&xz_log_quiet_obj)},
    {MP_ROM_QSTR(MP_QSTR_level), MP_ROM_PTR(&xz_log_level_obj)},
};
static MP_DEFINE_CONST_DICT(xz_log_locals_dict, xz_log_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(xz_log_type, MP_QSTR_Log, MP_TYPE_FLAG_NONE, make_new,
                                xz_log_make_new, locals_dict, &xz_log_locals_dict);

mp_obj_t xz_mpy_log_get(void) {
    xz_log_obj_t* o = mp_obj_malloc(xz_log_obj_t, &xz_log_type);
    return MP_OBJ_FROM_PTR(o);
}
