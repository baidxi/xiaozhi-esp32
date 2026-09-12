// Placeholder types for upstream MicroPython ESP32 port modules that are
// not compiled in the xiaozhi integration:
//
//   * esp32_ulp_type - ULP coprocessor support. IDF v6 only exposes the
//     FSM ULP headers on targets/configurations that enable the ULP
//     coprocessor, so upstream esp32_ulp.c cannot compile against the
//     xiaozhi sdkconfig.
//
//   * machine_touchpad_type - the legacy touch driver headers were removed
//     from IDF v6, and the assistant firmware owns the touch panel.
//
//   * esp32_pcnt_type - the legacy PCNT driver (driver/pcnt.h) was removed
//     from IDF v6 and replaced by an incompatible pulse-counter API.
//
// The upstream modesp32.c and modmachine.c reference these symbols
// unconditionally (guarded only by target/SOC macros), so the link needs
// these placeholders. Constructing them raises NotImplementedError.

#include "py/runtime.h"

static mp_obj_t xz_unsupported_make_new(const mp_obj_type_t* type, size_t n_args, size_t n_kw,
                                        const mp_obj_t* args) {
    (void)type;
    (void)n_args;
    (void)n_kw;
    (void)args;
    mp_raise_NotImplementedError(MP_ERROR_TEXT("not supported in the xiaozhi firmware"));
}

MP_DEFINE_CONST_OBJ_TYPE(esp32_ulp_type, MP_QSTR_ULP, MP_TYPE_FLAG_NONE, make_new,
                         xz_unsupported_make_new);

MP_DEFINE_CONST_OBJ_TYPE(machine_touchpad_type, MP_QSTR_TouchPad, MP_TYPE_FLAG_NONE, make_new,
                         xz_unsupported_make_new);

MP_DEFINE_CONST_OBJ_TYPE(esp32_pcnt_type, MP_QSTR_PCNT, MP_TYPE_FLAG_NONE, make_new,
                         xz_unsupported_make_new);
