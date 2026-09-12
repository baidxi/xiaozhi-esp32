// The `xiaozhi.wifi()` object (Python type `Wifi`): configure the assistant's
// WiFi station. Credentials are staged in C globals shared by every object
// (they survive soft resets, like the rest of the C registry).
//
//   wifi = xiaozhi.wifi()
//   wifi.ssid('my-ap')        # wifi.ssid() reads the staged value back
//   wifi.password('secret')
//   r = wifi.scan()           # ScanResult: len()/len/[i]/iteration
//   r.dump()                  # formatted table on the console
//   r.len(); r.foreach(cb)    # count / iterate (cb(item)); also for-in
//   r.tolist()                # plain list (json.dumps friendly)
//   wifi.connect()            # 'OK' / 'FALSE' (optional arg: timeout in ms)
//   wifi.save()               # persist for the assistant (used after reboot)
//   wifi.status()             # {'connected': True, 'ssid': ..., 'rssi': ...}
//
// connect() follows the firmware's own provisioning path (SsidManager +
// WifiManager.StartStation): on success the credentials are already
// persisted for the assistant; on failure the temporary entry is rolled
// back so the current network is unaffected.

#include "py/mperrno.h"
#include "py/runtime.h"

#include "xz_bridge.h"

#define XZ_WIFI_SSID_MAX 32     /* 802.11 SSID length limit */
#define XZ_WIFI_PASSWORD_MAX 64 /* WPA passphrase length limit */
#define XZ_WIFI_CONNECT_TIMEOUT_MS 15000

/* ---- Staged credentials (C globals: soft-reset safe, shared) ---- */

static char s_wifi_ssid[XZ_WIFI_SSID_MAX + 1] = {0};
static char s_wifi_password[XZ_WIFI_PASSWORD_MAX + 1] = {0};

static void wifi_check_registered(void) {
    if (xz_mpy_wifi_api() == NULL) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("no wifi on this board"));
    }
}

static void wifi_require_ssid(void) {
    if (s_wifi_ssid[0] == '\0') {
        mp_raise_ValueError(MP_ERROR_TEXT("ssid not set"));
    }
}

static mp_obj_t xz_wifi_make_new(const mp_obj_type_t* type, size_t n_args, size_t n_kw,
                                 const mp_obj_t* args) {
    (void)n_args;
    (void)n_kw;
    (void)args;
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    wifi_check_registered();
    mp_obj_base_t* o = mp_obj_malloc(mp_obj_base_t, type);
    return MP_OBJ_FROM_PTR(o);
}

/* ssid() -> staged str or None; ssid('name') -> None */
static mp_obj_t xz_wifi_ssid(size_t n_args, const mp_obj_t* args) {
    if (n_args == 1) {
        if (s_wifi_ssid[0] == '\0') {
            return mp_const_none;
        }
        return mp_obj_new_str(s_wifi_ssid, strlen(s_wifi_ssid));
    }
    size_t len = 0;
    const char* s = mp_obj_str_get_data(args[1], &len);
    if (len > XZ_WIFI_SSID_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("ssid too long (max 32)"));
    }
    memcpy(s_wifi_ssid, s, len);
    s_wifi_ssid[len] = '\0';
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(xz_wifi_ssid_obj, 1, xz_wifi_ssid);

/* password() -> staged str or None; password('secret') -> None */
static mp_obj_t xz_wifi_password(size_t n_args, const mp_obj_t* args) {
    if (n_args == 1) {
        if (s_wifi_password[0] == '\0') {
            return mp_const_none;
        }
        return mp_obj_new_str(s_wifi_password, strlen(s_wifi_password));
    }
    size_t len = 0;
    const char* s = mp_obj_str_get_data(args[1], &len);
    if (len > XZ_WIFI_PASSWORD_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("password too long (max 64)"));
    }
    memcpy(s_wifi_password, s, len);
    s_wifi_password[len] = '\0';
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(xz_wifi_password_obj, 1, xz_wifi_password);

static mp_obj_t xz_wifi_result(int rc) {
    if (rc == 0) {
        return mp_obj_new_str("OK", 2);
    }
    return mp_obj_new_str("FALSE", 5);
}

/* connect() or connect(timeout_ms) -> 'OK' / 'FALSE' (blocking) */
static mp_obj_t xz_wifi_connect(size_t n_args, const mp_obj_t* args) {
    mp_int_t timeout_ms = XZ_WIFI_CONNECT_TIMEOUT_MS;
    if (n_args >= 2) {
        timeout_ms = mp_obj_get_int(args[1]);
    }
    if (timeout_ms < 1000) {
        timeout_ms = 1000;
    }
    if (timeout_ms > 60000) {
        timeout_ms = 60000;
    }
    wifi_check_registered();
    wifi_require_ssid();
    int rc = xz_mpy_wifi_api()->connect(s_wifi_ssid, s_wifi_password, (int)timeout_ms);
    return xz_wifi_result(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(xz_wifi_connect_obj, 1, xz_wifi_connect);

/* ---- ScanResult: the scan() return value (list wrapper with dump()) ----
 *
 * Behaves like a list (len / [i] / iteration / print) and adds:
 *   r.dump()    formatted table on the console
 *   r.tolist()  the underlying plain list (json.dumps friendly)
 */
typedef struct _xz_scanlist_obj_t {
    mp_obj_base_t base;
    mp_obj_t items;  // the underlying list of dicts
} xz_scanlist_obj_t;

static void xz_scanlist_print(const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind) {
    xz_scanlist_obj_t* o = MP_OBJ_TO_PTR(self_in);
    mp_obj_print_helper(print, o->items, kind);
}

static mp_obj_t xz_scanlist_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
    xz_scanlist_obj_t* o = MP_OBJ_TO_PTR(self_in);
    if (op == MP_UNARY_OP_LEN) {
        return mp_obj_len(o->items);
    }
    if (op == MP_UNARY_OP_BOOL) {
        return mp_obj_new_bool(mp_obj_get_int(mp_obj_len(o->items)) > 0);
    }
    return MP_OBJ_NULL;
}

static mp_obj_t xz_scanlist_subscr(mp_obj_t self_in, mp_obj_t index, mp_obj_t value) {
    xz_scanlist_obj_t* o = MP_OBJ_TO_PTR(self_in);
    return mp_obj_subscr(o->items, index, value);
}

static mp_obj_t xz_scanlist_getiter(mp_obj_t self_in, mp_obj_iter_buf_t* iter_buf) {
    xz_scanlist_obj_t* o = MP_OBJ_TO_PTR(self_in);
    return mp_getiter(o->items, iter_buf);
}

/* dump() -> formatted table on the console */
static mp_obj_t xz_scanlist_dump(mp_obj_t self_in) {
    xz_scanlist_obj_t* o = MP_OBJ_TO_PTR(self_in);
    mp_int_t count = mp_obj_get_int(mp_obj_len(o->items));
    printf("scan: %d network%s\n", (int)count, count == 1 ? "" : "s");
    mp_obj_t iter = mp_getiter(o->items, NULL);
    mp_obj_t item;
    while ((item = mp_iternext(iter)) != MP_OBJ_STOP_ITERATION) {
        mp_obj_t ssid = mp_obj_dict_get(item, MP_OBJ_NEW_QSTR(MP_QSTR_ssid));
        mp_obj_t ch = mp_obj_dict_get(item, MP_OBJ_NEW_QSTR(MP_QSTR_channel));
        mp_obj_t rssi = mp_obj_dict_get(item, MP_OBJ_NEW_QSTR(MP_QSTR_rssi));
        mp_obj_t secure = mp_obj_dict_get(item, MP_OBJ_NEW_QSTR(MP_QSTR_secure));
        printf("  %-24s ch=%-3d rssi=%-4d dBm %s\n", mp_obj_str_get_str(ssid),
               (int)mp_obj_get_int(ch), (int)mp_obj_get_int(rssi),
               mp_obj_is_true(secure) ? "secure" : "open");
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_scanlist_dump_obj, xz_scanlist_dump);

/* tolist() -> the underlying plain list */
static mp_obj_t xz_scanlist_tolist(mp_obj_t self_in) {
    return ((xz_scanlist_obj_t*)MP_OBJ_TO_PTR(self_in))->items;
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_scanlist_tolist_obj, xz_scanlist_tolist);

/* len() -> network count (same as len(result)) */
static mp_obj_t xz_scanlist_len(mp_obj_t self_in) {
    return mp_obj_len(((xz_scanlist_obj_t*)MP_OBJ_TO_PTR(self_in))->items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_scanlist_len_obj, xz_scanlist_len);

/* foreach(callback) -> call callback(item) for every network; iteration
 * also works with `for ap in result:` */
static mp_obj_t xz_scanlist_foreach(mp_obj_t self_in, mp_obj_t callback) {
    xz_scanlist_obj_t* o = MP_OBJ_TO_PTR(self_in);
    mp_obj_t iter = mp_getiter(o->items, NULL);
    mp_obj_t item;
    while ((item = mp_iternext(iter)) != MP_OBJ_STOP_ITERATION) {
        mp_call_function_1(callback, item);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(xz_scanlist_foreach_obj, xz_scanlist_foreach);

static const mp_rom_map_elem_t xz_scanlist_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_dump), MP_ROM_PTR(&xz_scanlist_dump_obj)},
    {MP_ROM_QSTR(MP_QSTR_tolist), MP_ROM_PTR(&xz_scanlist_tolist_obj)},
    {MP_ROM_QSTR(MP_QSTR_len), MP_ROM_PTR(&xz_scanlist_len_obj)},
    {MP_ROM_QSTR(MP_QSTR_foreach), MP_ROM_PTR(&xz_scanlist_foreach_obj)},
};
static MP_DEFINE_CONST_DICT(xz_scanlist_locals_dict, xz_scanlist_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(xz_scanlist_type, MP_QSTR_ScanResult, MP_TYPE_FLAG_NONE, print,
                                xz_scanlist_print, unary_op, xz_scanlist_unary_op, subscr,
                                xz_scanlist_subscr, iter, xz_scanlist_getiter, locals_dict,
                                &xz_scanlist_locals_dict);

/* scan() -> ScanResult (deduplicated, RSSI order) */
static mp_obj_t xz_wifi_scan(mp_obj_t self_in) {
    (void)self_in;
    wifi_check_registered();
    const xz_mpy_wifi_api_t* api = xz_mpy_wifi_api();
    if (api->scan == NULL) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("wifi scan not supported"));
    }
    xz_mpy_wifi_ap_t aps[24];
    int n = api->scan(aps, 24);
    if (n < 0) {
        mp_raise_OSError(MP_EIO);
    }
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < n; i++) {
        mp_obj_t dict = mp_obj_new_dict(4);
        mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(MP_QSTR_ssid),
                          mp_obj_new_str(aps[i].ssid, strlen(aps[i].ssid)));
        mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(MP_QSTR_secure),
                          mp_obj_new_bool(aps[i].secure));
        mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(MP_QSTR_channel),
                          mp_obj_new_int(aps[i].channel));
        mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(MP_QSTR_rssi),
                          mp_obj_new_int(aps[i].rssi));
        mp_obj_list_append(MP_OBJ_FROM_PTR(list), MP_OBJ_FROM_PTR(dict));
    }
    xz_scanlist_obj_t* o = mp_obj_malloc(xz_scanlist_obj_t, &xz_scanlist_type);
    o->items = list;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_wifi_scan_obj, xz_wifi_scan);

/* save() -> 'OK' / 'FALSE' (persist without connecting) */
static mp_obj_t xz_wifi_save(mp_obj_t self_in) {
    (void)self_in;
    wifi_check_registered();
    wifi_require_ssid();
    int rc = xz_mpy_wifi_api()->save(s_wifi_ssid, s_wifi_password);
    return xz_wifi_result(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_wifi_save_obj, xz_wifi_save);

/* status() -> {'connected', 'ssid', 'ip', 'rssi'} */
static mp_obj_t xz_wifi_status(mp_obj_t self_in) {
    (void)self_in;
    wifi_check_registered();
    const xz_mpy_wifi_api_t* api = xz_mpy_wifi_api();
    char buf[XZ_WIFI_SSID_MAX + 1];

    mp_obj_t dict = mp_obj_new_dict(4);
    bool connected = api->is_connected != NULL && api->is_connected();
    mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(MP_QSTR_connected),
                      mp_obj_new_bool(connected));

    buf[0] = '\0';
    if (api->get_ssid != NULL) {
        api->get_ssid(buf, sizeof(buf));
    }
    mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(MP_QSTR_ssid),
                      buf[0] != '\0' ? mp_obj_new_str(buf, strlen(buf)) : mp_const_none);

    buf[0] = '\0';
    if (api->get_ip != NULL) {
        api->get_ip(buf, sizeof(buf));
    }
    mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(MP_QSTR_ip),
                      buf[0] != '\0' ? mp_obj_new_str(buf, strlen(buf)) : mp_const_none);

    mp_int_t rssi = api->get_rssi != NULL ? api->get_rssi() : 0;
    mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(MP_QSTR_rssi), mp_obj_new_int(rssi));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(xz_wifi_status_obj, xz_wifi_status);

static const mp_rom_map_elem_t xz_wifi_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_ssid), MP_ROM_PTR(&xz_wifi_ssid_obj)},
    {MP_ROM_QSTR(MP_QSTR_password), MP_ROM_PTR(&xz_wifi_password_obj)},
    {MP_ROM_QSTR(MP_QSTR_scan), MP_ROM_PTR(&xz_wifi_scan_obj)},
    {MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&xz_wifi_connect_obj)},
    {MP_ROM_QSTR(MP_QSTR_save), MP_ROM_PTR(&xz_wifi_save_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&xz_wifi_status_obj)},
};
static MP_DEFINE_CONST_DICT(xz_wifi_locals_dict, xz_wifi_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(xz_wifi_type, MP_QSTR_Wifi, MP_TYPE_FLAG_NONE, make_new,
                                xz_wifi_make_new, locals_dict, &xz_wifi_locals_dict);

// Exported accessor for the xiaozhi module (keeps the type file-local).
mp_obj_t xz_mpy_wifi_get(void) {
    if (xz_mpy_wifi_api() == NULL) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("no wifi on this board"));
    }
    mp_obj_base_t* o = mp_obj_malloc(mp_obj_base_t, &xz_wifi_type);
    return MP_OBJ_FROM_PTR(o);
}
