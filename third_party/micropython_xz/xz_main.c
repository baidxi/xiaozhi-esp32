// MicroPython task entry for the xiaozhi firmware integration.
//
// This is an adapted copy of upstream ports/esp32/main.c for ESP-IDF v6:
//   * esp_flash_t became opaque; esp_flash_default_chip->size accesses are
//     replaced with esp_flash_get_size().
//   * The catch-all "vfs" FAT partition registration was removed: the
//     xiaozhi partition table is fully allocated and the runtime uses a
//     dedicated littlefs partition (stage 2).
//   * esp_native_code_commit() handles builds where MALLOC_CAP_EXEC is not
//     available (PMP IDRAM split).
// The REPL/soft-reset flow is unchanged from upstream.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "extmod/modmachine.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "py/nlr.h"
#include "py/persistentcode.h"
#include "py/repl.h"
#include "py/runtime.h"
#include "shared/readline/readline.h"
#include "shared/runtime/pyexec.h"

#include "esp_netif_types.h"

// esp_interface_t belonged to the removed tcpip_adapter API and no longer
// exists in IDF v6; the networking-free build only needs the type so the
// upstream modnetwork.h parses.
#ifndef ESP_IDF_VERSION_VAL
#include "esp_idf_version.h"
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) && !defined(__DOXYGEN__)
typedef enum {
    ESP_IF_WIFI_STA = 0,
    ESP_IF_WIFI_AP,
    ESP_IF_ETH,
    ESP_IF_MAX,
} esp_interface_t;
#endif

#include "modesp32.h"
#include "modmachine.h"
#include "modnetwork.h"
#include "uart.h"
#include "usb_serial_jtag.h"

// MicroPython runs as a task under FreeRTOS
#define MP_TASK_PRIORITY (ESP_TASK_PRIO_MIN + 1)

typedef struct _native_code_node_t {
    struct _native_code_node_t* next;
    uint32_t data[];
} native_code_node_t;

static native_code_node_t* native_code_head = NULL;

static void esp_native_code_free_all(void);

int vprintf_null(const char* format, va_list ap) {
    // do nothing: this is used as a log target during raw repl mode
    (void)format;
    (void)ap;
    return 0;
}

void mp_task(void* pvParameter) {
    volatile uint32_t sp = (uint32_t)esp_cpu_get_sp();
    (void)pvParameter;
#if MICROPY_PY_THREAD
    mp_thread_init(pxTaskGetStackStart(NULL), MICROPY_TASK_STACK_SIZE / sizeof(uintptr_t));
#endif
#if MICROPY_HW_ESP_USB_SERIAL_JTAG
    usb_serial_jtag_init();
#endif
#if MICROPY_HW_ENABLE_UART_REPL
    uart_stdout_init();
#endif
    machine_init();

    // The assistant firmware already created the default event loop; a
    // duplicate creation only returns an error, which is safe to ignore.
    esp_event_loop_create_default();

    void* mp_task_heap = MP_PLAT_ALLOC_HEAP(MICROPY_GC_INITIAL_HEAP_SIZE);
    if (mp_task_heap == NULL) {
        printf("mp_task_heap allocation failed!\n");
        esp_restart();
    }

soft_reset:
    // initialise the stack pointer for the main thread
    mp_cstack_init_with_top((void*)sp, MICROPY_TASK_STACK_SIZE);
    gc_init(mp_task_heap, mp_task_heap + MICROPY_GC_INITIAL_HEAP_SIZE);
    mp_init();
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_lib));
    readline_init0();

    // initialise peripherals
    machine_pins_init();
#if MICROPY_PY_MACHINE_I2S
    machine_i2s_init0();
#endif

    int ret = pyexec_file_if_exists("boot.py");

    if (ret & PYEXEC_FORCED_EXIT) {
        goto soft_reset_exit;
    }
    if (pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL && ret != 0) {
        int ret = pyexec_file_if_exists("main.py");
        if (ret & PYEXEC_FORCED_EXIT) {
            goto soft_reset_exit;
        }
    }

    for (;;) {
        if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
            vprintf_like_t vprintf_log = esp_log_set_vprintf(vprintf_null);
            if (pyexec_raw_repl() != 0) {
                break;
            }
            esp_log_set_vprintf(vprintf_log);
        } else {
            if (pyexec_friendly_repl() != 0) {
                break;
            }
        }
    }

soft_reset_exit:

#if MICROPY_PY_MACHINE_UART
    machine_uart_deinit_all();
#endif

#if MICROPY_PY_THREAD
    mp_thread_deinit();
#endif

    gc_sweep_all();

    // Free any native code pointers that point to iRAM.
    esp_native_code_free_all();

    mp_hal_stdout_tx_str("MPY: soft reboot\r\n");

// deinitialise peripherals
#if MICROPY_PY_MACHINE_PWM
    machine_pwm_deinit_all();
#endif
    machine_pins_deinit();
    machine_deinit();

    mp_deinit();
    fflush(stdout);

    goto soft_reset;
}

void xz_micropython_boot(void) {
    // Hook for board startup (xz_board_startup replaces the upstream
    // boardctrl_startup which would re-initialise NVS and register a
    // catch-all FAT partition).
    MICROPY_BOARD_STARTUP();

    // Create and transfer control to the MicroPython task. The task handle
    // must be stored in mp_main_task_handle (defined in mphalport.c): the
    // REPL RX interrupts wake the task through it.
    extern TaskHandle_t mp_main_task_handle;
    xTaskCreatePinnedToCore(mp_task, "mp_task", MICROPY_TASK_STACK_SIZE / sizeof(StackType_t), NULL,
                            MP_TASK_PRIORITY, &mp_main_task_handle, MP_TASK_COREID);
}

void nlr_jump_fail(void* val) {
    printf("NLR jump failed, val=%p\n", val);
    esp_restart();
}

static void esp_native_code_free_all(void) {
    while (native_code_head != NULL) {
        native_code_node_t* next = native_code_head->next;
        heap_caps_free(native_code_head);
        native_code_head = next;
    }
}

void* esp_native_code_commit(void* buf, size_t len, void* reloc) {
    len = (len + 3) & ~3;
    size_t len_node = sizeof(native_code_node_t) + len;
    native_code_node_t* node = NULL;
#ifdef MALLOC_CAP_EXEC
    node = heap_caps_malloc(len_node, MALLOC_CAP_EXEC);
#else
    // PMP IDRAM split: external executable memory is unavailable, so the
    // native/viper emitters cannot be used; plain Python is unaffected.
    node = NULL;
#endif
    if (node == NULL) {
        m_malloc_fail(len_node);
    }
    node->next = native_code_head;
    native_code_head = node;
    void* p = node->data;
    if (reloc) {
        mp_native_relocate(reloc, buf, (uintptr_t)p);
    }
    memcpy(p, buf, len);
    return p;
}
