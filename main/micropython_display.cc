// LVGL 9 overlay canvas implementing the xiaozhi MicroPython display API.
//
// The assistant UI owns the screen; Python draws on a full-screen canvas
// that is created lazily on first use, raised above the chat UI, and can
// be hidden again with display().hide(). All drawing runs on the caller's
// context under the LVGL port lock (esp_lvgl_port), serialising access
// with the assistant UI task.
//
// LVGL 9 canvas drawing model: lv_canvas_init_layer() then lv_draw_*()
// into the layer, then lv_canvas_finish_layer() blits it into the canvas
// buffer.

#include "board.h"
#include "display/display.h"
#include "xz_bridge.h"

#if CONFIG_XIAOZHI_MICROPYTHON

#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include "board.h"
#include "display/display.h"

namespace {

lv_obj_t* s_canvas = nullptr;
lv_color_t* s_canvas_buf = nullptr;
int s_screen_w = 0;
int s_screen_h = 0;

lv_color_t to_lv_color(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

bool EnsureCanvas() {
    if (s_canvas != nullptr) {
        return true;
    }
    Display* display = Board::GetInstance().GetDisplay();
    if (display == nullptr || display->width() <= 0) {
        return false;
    }
    s_screen_w = display->width();
    s_screen_h = display->height();

    s_canvas_buf = (lv_color_t*)heap_caps_malloc(s_screen_w * s_screen_h * sizeof(lv_color_t),
                                                 MALLOC_CAP_SPIRAM);
    if (s_canvas_buf == nullptr) {
        return false;
    }

    // Must be created on the LVGL task; the first call may come from the
    // mp_task, so wrap creation in the port lock.
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) {
        heap_caps_free(s_canvas_buf);
        s_canvas_buf = nullptr;
        return false;
    }
    lv_obj_t* parent = lv_screen_active() != nullptr ? lv_screen_active() : lv_layer_top();
    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, s_screen_w, s_screen_h, LV_COLOR_FORMAT_NATIVE);
    lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_size(s_canvas, s_screen_w, s_screen_h);
    lv_obj_move_foreground(s_canvas);
    lvgl_port_unlock();
    return true;
}

// RAII helper: lock + layer init/finish per drawing call.
struct DrawSession {
    lv_layer_t layer;
    bool ok;
    DrawSession() : ok(false) {
        if (lvgl_port_lock(pdMS_TO_TICKS(200)) && EnsureCanvas()) {
            lv_canvas_init_layer(s_canvas, &layer);
            ok = true;
        }
    }
    ~DrawSession() {
        if (ok) {
            lv_canvas_finish_layer(s_canvas, &layer);
            lvgl_port_unlock();
        }
    }
};

// Text label registry (declared before use in clear()).
#define XZ_MAX_LABELS 32
lv_obj_t* s_labels[XZ_MAX_LABELS] = {nullptr};
int s_label_count = 0;

void XzDispClear(uint32_t color) {
    if (lvgl_port_lock(pdMS_TO_TICKS(200)) && EnsureCanvas()) {
        // Remove any text labels drawn on the canvas.
        for (int i = 0; i < s_label_count; i++) {
            lv_obj_delete(s_labels[i]);
        }
        s_label_count = 0;
        lv_canvas_fill_bg(s_canvas, to_lv_color(color), LV_OPA_COVER);
        lvgl_port_unlock();
    }
}

void XzDispPixel(int x, int y, uint32_t color) {
    if (lvgl_port_lock(pdMS_TO_TICKS(200)) && EnsureCanvas()) {
        if (x >= 0 && x < s_screen_w && y >= 0 && y < s_screen_h) {
            s_canvas_buf[y * s_screen_w + x] = to_lv_color(color);
            lv_obj_invalidate(s_canvas);
        }
        lvgl_port_unlock();
    }
}

void XzDispLine(int x1, int y1, int x2, int y2, uint32_t color) {
    DrawSession s;
    if (!s.ok)
        return;
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1.x = (int32_t)x1;
    dsc.p1.y = (int32_t)y1;
    dsc.p2.x = (int32_t)x2;
    dsc.p2.y = (int32_t)y2;
    dsc.color = to_lv_color(color);
    dsc.width = 2;
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end = 1;
    lv_draw_line(&s.layer, &dsc);
}

void XzDispRect(int x, int y, int w, int h, uint32_t color, bool fill) {
    DrawSession s;
    if (!s.ok)
        return;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = to_lv_color(color);
    dsc.bg_opa = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
    dsc.border_color = to_lv_color(color);
    dsc.border_opa = LV_OPA_COVER;
    dsc.border_width = 2;
    dsc.radius = 0;
    lv_area_t coords = {
        .x1 = (int32_t)x, .y1 = (int32_t)y, .x2 = (int32_t)(x + w - 1), .y2 = (int32_t)(y + h - 1)};
    lv_draw_rect(&s.layer, &dsc, &coords);
}

void XzDispCircle(int cx, int cy, int r, uint32_t color, bool fill) {
    DrawSession s;
    if (!s.ok)
        return;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = to_lv_color(color);
    dsc.bg_opa = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
    dsc.border_color = to_lv_color(color);
    dsc.border_opa = LV_OPA_COVER;
    dsc.border_width = 2;
    dsc.radius = LV_RADIUS_CIRCLE;
    lv_area_t coords = {.x1 = (int32_t)(cx - r),
                        .y1 = (int32_t)(cy - r),
                        .x2 = (int32_t)(cx + r - 1),
                        .y2 = (int32_t)(cy + r - 1)};
    lv_draw_rect(&s.layer, &dsc, &coords);
}

// Text is drawn as lv_label children of the canvas (see s_labels above).
void XzDispText(int x, int y, const char* utf8, uint32_t color) {
    if (!lvgl_port_lock(pdMS_TO_TICKS(200)) || !EnsureCanvas()) {
        return;
    }
    if (s_label_count < XZ_MAX_LABELS) {
        lv_obj_t* label = lv_label_create(s_canvas);
        lv_label_set_text(label, utf8);
        lv_obj_set_style_text_color(label, to_lv_color(color), 0);
        lv_obj_set_pos(label, x, y);
        s_labels[s_label_count++] = label;
    }
    lvgl_port_unlock();
}

void XzDispSize(int* out_w, int* out_h) {
    if (!EnsureCanvas()) {
        *out_w = 0;
        *out_h = 0;
        return;
    }
    *out_w = s_screen_w;
    *out_h = s_screen_h;
}

void XzDispHide(void) {
    if (s_canvas == nullptr) {
        return;
    }
    if (lvgl_port_lock(pdMS_TO_TICKS(200))) {
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

const xz_mpy_display_api_t kDisplayApi = {
    .clear = XzDispClear,
    .pixel = XzDispPixel,
    .line = XzDispLine,
    .rect = XzDispRect,
    .circle = XzDispCircle,
    .text = XzDispText,
    .size = XzDispSize,
    .hide = XzDispHide,
};

}  // namespace

// Implemented in third_party/micropython_xz/xz_xiaozhi.c.
void xz_mpy_display_register(const xz_mpy_display_api_t* api);

// Called from main.cc after Application::Initialize().
extern "C" void xz_micropython_register_display(void) { xz_mpy_display_register(&kDisplayApi); }

#endif  // CONFIG_XIAOZHI_MICROPYTHON
