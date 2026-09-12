// The `xiaozhi.screen()` object (Python type `Screen`): drawing on the board
// screen via bridge callbacks. The implementation (LVGL overlay canvas) lives in the main
// component, which owns the display; this file only marshals Python calls
// through the callback table in xz_bridge.h. Colour arguments accept
// 0xRRGGBB ints or one of a few common names.

#include "py/runtime.h"

#include "xz_bridge.h"

static mp_obj_t xz_disp_make_new(const mp_obj_type_t* type, size_t n_args, size_t n_kw,
                                 const mp_obj_t* args) {
    (void)n_args;
    (void)n_kw;
    (void)args;
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    const xz_mpy_display_api_t* api = xz_mpy_display_api();
    if (api == NULL) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("no display on this board"));
    }
    mp_obj_base_t* o = mp_obj_malloc(mp_obj_base_t, type);
    return MP_OBJ_FROM_PTR(o);
}

static uint32_t xz_disp_color(mp_obj_t color_in) {
    if (mp_obj_is_str(color_in)) {
        const char* name = mp_obj_str_get_str(color_in);
        // Common colour names for block programming.
        if (strcmp(name, "black") == 0)
            return 0x000000;
        if (strcmp(name, "white") == 0)
            return 0xFFFFFF;
        if (strcmp(name, "red") == 0)
            return 0xFF0000;
        if (strcmp(name, "green") == 0)
            return 0x00FF00;
        if (strcmp(name, "blue") == 0)
            return 0x0000FF;
        if (strcmp(name, "yellow") == 0)
            return 0xFFFF00;
        if (strcmp(name, "cyan") == 0)
            return 0x00FFFF;
        if (strcmp(name, "magenta") == 0)
            return 0xFF00FF;
        if (strcmp(name, "orange") == 0)
            return 0xFFA500;
        if (strcmp(name, "gray") == 0 || strcmp(name, "grey") == 0)
            return 0x808080;
        if (name[0] == '#') {
            // #RRGGBB
            return (uint32_t)strtoul(name + 1, NULL, 16);
        }
        mp_raise_ValueError(MP_ERROR_TEXT("unknown colour"));
    }
    return (uint32_t)mp_obj_get_int_truncated(color_in);
}

static void xz_disp_check(void) {
    if (xz_mpy_display_api() == NULL) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("no display on this board"));
    }
}

static mp_obj_t xz_disp_clear(mp_obj_t self_in, mp_obj_t color_in) {
    (void)self_in;
    xz_disp_check();
    xz_mpy_display_api()->clear(xz_disp_color(color_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(xz_disp_clear_obj, xz_disp_clear);

static mp_obj_t xz_disp_pixel(size_t n_args, const mp_obj_t* args) {
    xz_disp_check();
    xz_mpy_display_api()->pixel(mp_obj_get_int(args[1]), mp_obj_get_int(args[2]),
                                xz_disp_color(args[3]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(xz_disp_pixel_obj, 4, xz_disp_pixel);

static mp_obj_t xz_disp_line(size_t n_args, const mp_obj_t* args) {
    xz_disp_check();
    xz_mpy_display_api()->line(mp_obj_get_int(args[1]), mp_obj_get_int(args[2]),
                               mp_obj_get_int(args[3]), mp_obj_get_int(args[4]),
                               xz_disp_color(args[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(xz_disp_line_obj, 6, xz_disp_line);

static mp_obj_t xz_disp_rect(size_t n_args, const mp_obj_t* args) {
    xz_disp_check();
    bool fill = n_args >= 7 && mp_obj_is_true(args[6]);
    xz_mpy_display_api()->rect(mp_obj_get_int(args[1]), mp_obj_get_int(args[2]),
                               mp_obj_get_int(args[3]), mp_obj_get_int(args[4]),
                               xz_disp_color(args[5]), fill);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(xz_disp_rect_obj, 6, xz_disp_rect);

static mp_obj_t xz_disp_circle(size_t n_args, const mp_obj_t* args) {
    xz_disp_check();
    bool fill = n_args >= 6 && mp_obj_is_true(args[5]);
    xz_mpy_display_api()->circle(mp_obj_get_int(args[1]), mp_obj_get_int(args[2]),
                                 mp_obj_get_int(args[3]), xz_disp_color(args[4]), fill);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(xz_disp_circle_obj, 5, xz_disp_circle);

static mp_obj_t xz_disp_text(size_t n_args, const mp_obj_t* args) {
    xz_disp_check();
    xz_mpy_display_api()->text(mp_obj_get_int(args[1]), mp_obj_get_int(args[2]),
                               mp_obj_str_get_str(args[3]), xz_disp_color(args[4]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(xz_disp_text_obj, 5, xz_disp_text);

static mp_obj_t xz_disp_size(mp_obj_t self_in) {
    (void)self_in;
    xz_disp_check();
    int w = 0, h = 0;
    xz_mpy_display_api()->size(&w, &h);
    mp_obj_t tuple[2] = {mp_obj_new_int(w), mp_obj_new_int(h)};
    return mp_obj_new_tuple(2, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_disp_size_obj, xz_disp_size);

static mp_obj_t xz_disp_hide(mp_obj_t self_in) {
    (void)self_in;
    xz_disp_check();
    xz_mpy_display_api()->hide();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_disp_hide_obj, xz_disp_hide);

static const mp_rom_map_elem_t xz_disp_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&xz_disp_clear_obj)},
    {MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&xz_disp_pixel_obj)},
    {MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&xz_disp_line_obj)},
    {MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&xz_disp_rect_obj)},
    {MP_ROM_QSTR(MP_QSTR_circle), MP_ROM_PTR(&xz_disp_circle_obj)},
    {MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&xz_disp_text_obj)},
    {MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&xz_disp_size_obj)},
    {MP_ROM_QSTR(MP_QSTR_hide), MP_ROM_PTR(&xz_disp_hide_obj)},
};
static MP_DEFINE_CONST_DICT(xz_disp_locals_dict, xz_disp_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(xz_screen_type, MP_QSTR_Screen, MP_TYPE_FLAG_NONE, make_new,
                                xz_disp_make_new, locals_dict, &xz_disp_locals_dict);

// Exported accessor for the xiaozhi module (keeps the type file-local).
mp_obj_t xz_mpy_display_get(void) {
    const xz_mpy_display_api_t* api = xz_mpy_display_api();
    if (api == NULL) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("no display on this board"));
    }
    mp_obj_base_t* o = mp_obj_malloc(mp_obj_base_t, &xz_screen_type);
    return MP_OBJ_FROM_PTR(o);
}
