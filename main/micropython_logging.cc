// Log capture for the xiaozhi MicroPython bridge.
//
// Installs an esp_log vprintf hook that mirrors every log line into a
// PSRAM ring buffer while (optionally) still printing to the console.
// Python reads the buffer via xiaozhi.log.read() and can silence the
// console with xiaozhi.log.quiet(True) so the REPL stays clean.
//
// Compat: upstream MicroPython's raw-REPL mode calls esp_log_set_vprintf
// itself and restores the previous hook on exit; because we install our
// hook at startup, their save/restore correctly round-trips around us.

#include "sdkconfig.h"

#if CONFIG_XIAOZHI_MICROPYTHON

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"

namespace {

constexpr int kLogRingSize = 8 * 1024;  // PSRAM ring buffer
char* s_ring = nullptr;
int s_head = 0;  // write position (guarded by s_lock)
int s_stored = 0;
SemaphoreHandle_t s_lock = nullptr;
volatile bool s_console_quiet = false;
vprintf_like_t s_orig_vprintf = nullptr;

int XzLogVprintf(const char* format, va_list args) {
    // Format once, then mirror + optionally print.
    char line[256];
    int n = vsnprintf(line, sizeof(line), format, args);
    if (n <= 0) {
        return 0;
    }

    if (s_lock != nullptr && xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        int len = n < (int)sizeof(line) - 1 ? n : (int)sizeof(line) - 1;
        for (int i = 0; i < len; i++) {
            s_ring[s_head] = line[i];
            s_head = (s_head + 1) % kLogRingSize;
            if (s_stored < kLogRingSize) {
                s_stored++;
            }
        }
        // Keep line semantics: append newline if the writer didn't.
        if (len == 0 || line[len - 1] != '\n') {
            s_ring[s_head] = '\n';
            s_head = (s_head + 1) % kLogRingSize;
            if (s_stored < kLogRingSize) {
                s_stored++;
            }
        }
        xSemaphoreGive(s_lock);
    }

    if (!s_console_quiet && s_orig_vprintf != nullptr) {
        va_list copy;
        va_copy(copy, args);
        s_orig_vprintf(format, copy);
        va_end(copy);
    }
    return n;
}

}  // namespace

// C API for the bridge (component side stays free of esp_log internals).

extern "C" {

// Install the hook (called once from main.cc before the mpy runtime).
void xz_micropython_log_init(void) {
    s_ring = (char*)heap_caps_malloc(kLogRingSize, MALLOC_CAP_SPIRAM);
    if (s_ring == nullptr) {
        return;
    }
    memset(s_ring, 0, kLogRingSize);
    s_lock = xSemaphoreCreateMutex();
    s_orig_vprintf = esp_log_set_vprintf(XzLogVprintf);
}

// Read (and consume) buffered lines; returns bytes copied (0 if empty).
int xz_micropython_log_read(char* buf, int buf_size) {
    if (s_ring == nullptr || s_lock == nullptr) {
        return 0;
    }
    int copied = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        int start = (s_head - s_stored + kLogRingSize) % kLogRingSize;
        while (s_stored > 0 && copied < buf_size - 1) {
            buf[copied++] = s_ring[start];
            start = (start + 1) % kLogRingSize;
            s_stored--;
        }
        buf[copied] = '\0';
        xSemaphoreGive(s_lock);
    }
    return copied;
}

// Drop buffered content.
void xz_micropython_log_clear(void) {
    if (s_lock != nullptr && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_stored = 0;
        xSemaphoreGive(s_lock);
    }
}

// Console silence toggle (buffer capture always continues).
void xz_micropython_log_quiet(int enable) { s_console_quiet = enable != 0; }

// Global log level (esp_log_level_set("*", ...)).
void xz_micropython_log_level(int level) {
    static const esp_log_level_t kLevels[] = {ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN,
                                              ESP_LOG_INFO, ESP_LOG_DEBUG};
    if (level >= 0 && level <= 4) {
        esp_log_level_set("*", kLevels[level]);
    }
}

}  // extern "C"

#endif  // CONFIG_XIAOZHI_MICROPYTHON
