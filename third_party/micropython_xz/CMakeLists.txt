# MicroPython runtime component for the xiaozhi firmware.
#
# Builds the upstream MicroPython sources from the third_party/micropython
# git submodule with a feature set trimmed for coexistence with the xiaozhi
# assistant:
#   * No networking / Bluetooth / ESPNOW / sockets / SSL: the assistant
#     firmware owns the network stacks.
#   * No TinyUSB / USB-serial-JTAG: the console is managed by ESP-IDF.
#   * No machine.I2S (audio codec owns the I2S peripherals) and no SD card.
#   * machine.UART is kept: user scripts talk to an external STM32
#     execution controller over a UART link.
#   * littlefs VFS is kept for the python script partition (stage 2).
#
# This follows the out-of-tree pattern documented at the top of
# ports/esp32/esp32_common.cmake, but registers a trimmed source/component
# list instead of including that file wholesale.

# Location of the upstream MicroPython checkout (git submodule).
# NOTE: use CMAKE_CURRENT_LIST_DIR, not CMAKE_SOURCE_DIR. During the IDF
# component-requirements pass (component_get_requirements.cmake) this file
# is included from a temporary script in the build directory, where
# CMAKE_SOURCE_DIR points at build/, not at the project root.
if(NOT MICROPY_DIR)
    get_filename_component(MICROPY_DIR ${CMAKE_CURRENT_LIST_DIR}/../micropython ABSOLUTE)
endif()
if(NOT EXISTS ${MICROPY_DIR}/py/py.cmake)
    message(FATAL_ERROR
        "MicroPython sources not found at ${MICROPY_DIR}. "
        "Run: git submodule update --init third_party/micropython")
endif()

set(MICROPY_PORT_DIR ${MICROPY_DIR}/ports/esp32)
set(MICROPY_BOARD_DIR ${CMAKE_CURRENT_LIST_DIR}/boards/xiaozhi)

# Feature trims on the CMake side (mirrors boards/xiaozhi/mpconfigboard.h).
set(MICROPY_PY_TINYUSB OFF)
set(MICROPY_PY_BTREE OFF)

# RISC-V targets need the native gchelper sources.
if(CONFIG_IDF_TARGET_ARCH_RISCV)
    list(APPEND MICROPY_SOURCE_LIB
        ${MICROPY_DIR}/shared/runtime/gchelper_native.c
        ${MICROPY_DIR}/shared/runtime/gchelper_rv32i.s
    )
endif()

# Core py/ definitions and helper functions.
include(${MICROPY_DIR}/py/py.cmake)

# CMAKE_BUILD_EARLY_EXPANSION is set during the component-discovery phase of
# `idf.py build`, so the extmod/usermod rules must be skipped then (mirrors
# the upstream esp32_common.cmake logic).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(${MICROPY_DIR}/py/usermod.cmake)
    include(${MICROPY_DIR}/extmod/extmod.cmake)
endif()

list(APPEND MICROPY_QSTRDEFS_PORT
    ${MICROPY_PORT_DIR}/qstrdefsport.h
)

list(APPEND MICROPY_SOURCE_SHARED
    ${MICROPY_DIR}/shared/readline/readline.c
    ${MICROPY_DIR}/shared/netutils/netutils.c
    ${MICROPY_DIR}/shared/timeutils/timeutils.c
    ${MICROPY_DIR}/shared/runtime/interrupt_char.c
    ${MICROPY_DIR}/shared/runtime/mpirq.c
    ${MICROPY_DIR}/shared/runtime/stdout_helpers.c
    ${MICROPY_DIR}/shared/runtime/sys_stdio_mphal.c
    ${MICROPY_DIR}/shared/runtime/pyexec.c
)

# littlefs is the VFS for the python script partition. oofatfs sources are
# kept because extmod/modos.c includes oofatfs/ff.h whenever
# MICROPY_PY_OS_SYNC is enabled (the esp32 port enables it).
list(APPEND MICROPY_SOURCE_LIB
    ${MICROPY_DIR}/lib/littlefs/lfs2.c
    ${MICROPY_DIR}/lib/littlefs/lfs2_util.c
    ${MICROPY_DIR}/lib/oofatfs/ff.c
    ${MICROPY_DIR}/lib/oofatfs/ffunicode.c
)

list(APPEND MICROPY_SOURCE_DRIVERS
    ${MICROPY_DIR}/drivers/bus/softspi.c
    ${MICROPY_DIR}/drivers/dht/dht.c
)

# Trimmed port sources. main.c is replaced by xz_main.c (ESP-IDF v6 flash
# API adaptations, no catch-all vfs partition); machine_touchpad.c,
# esp32_ulp.c and esp32_pcnt.c are replaced by xz_stubs.c placeholders.
# Compared to the upstream MICROPY_SOURCE_PORT list this removes:
# network_wlan.c, network_wlan_csi.c, modsocket.c, lwip_patch.c,
# mpnimbleport.c, modespnow.c, usb.c, machine_sdcard.c, ppp_set_auth.c and
# fatfs_port.c. The machine_pwm/adc/uart/wdt/i2c(i2s)/... implementations
# are pulled in via the MICROPY_PY_MACHINE_*_INCLUDEFILE mechanism and are
# not listed here.
list(APPEND MICROPY_SOURCE_PORT
    uart.c
    usb_serial_jtag.c
    gccollect.c
    mphalport.c
    panichandler.c
    help.c
    adc.c
    machine_bitstream.c
    machine_timer.c
    machine_pin.c
    machine_dac.c
    machine_i2c.c
    machine_hw_spi.c
    fatfs_port.c
    mpthreadport.c
    machine_rtc.c
    modesp.c
    modesp32.c
    esp32_nvs.c
    esp32_partition.c
    esp32_rmt.c
    esp32_ldo.c
)
list(TRANSFORM MICROPY_SOURCE_PORT PREPEND ${MICROPY_PORT_DIR}/)
list(APPEND MICROPY_SOURCE_PORT ${CMAKE_BINARY_DIR}/pins.c)

list(APPEND MICROPY_SOURCE_QSTR
    ${MICROPY_SOURCE_PY}
    ${MICROPY_SOURCE_EXTMOD}
    ${MICROPY_SOURCE_USERMOD}
    ${MICROPY_SOURCE_SHARED}
    ${MICROPY_SOURCE_LIB}
    ${MICROPY_SOURCE_PORT}
    ${MICROPY_SOURCE_BOARD}
    ${CMAKE_CURRENT_LIST_DIR}/xz_xiaozhi.c
    ${CMAKE_CURRENT_LIST_DIR}/xz_display.c
    ${CMAKE_CURRENT_LIST_DIR}/xz_wifi.c
    ${CMAKE_CURRENT_LIST_DIR}/xz_log.c
    ${CMAKE_CURRENT_LIST_DIR}/drivers/qmi8658.c
    ${CMAKE_CURRENT_LIST_DIR}/drivers/pcf85063.c
)

# Trimmed IDF component dependencies: no bt / esp_eth / esp_netif /
# esp_wifi / lwip / sdmmc / usb from the upstream list.
list(APPEND MICROPY_XZ_IDF_COMPONENTS
    app_update
    bootloader_support
    driver
    esp_adc
    esp_app_format
    esp_common
    esp_driver_gptimer
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_ledc
    esp_driver_rmt
    esp_driver_spi
    esp_driver_tsens
    esp_driver_uart
    esp_event
    esp_hw_support
    esp_mm
    esp_netif
    esp_partition
    esp_pm
    esp_psram
    esp_ringbuf
    esp_rom
    esp_system
    esp_wifi
    esp_timer
    freertos
    hal
    heap
    log
    mbedtls
    esp_libc
    nvs_flash
    soc
    spi_flash
    vfs
)

# ESP-IDF v6 moved the peripheral maps from soc/<p>_periph.h to
# esp_hal_*/include/hal/<p>_periph.h; upstream MicroPython still uses the
# old paths. compat/idf6 provides forwarding headers (IDF < 6 does not need
# them).
set(MICROPY_XZ_INCLUDE_DIRS "")
if($ENV{IDF_VERSION} VERSION_GREATER_EQUAL "6")
    list(APPEND MICROPY_XZ_INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/compat/idf6)
endif()

idf_component_register(
    SRCS
        ${MICROPY_SOURCE_PY}
        ${MICROPY_SOURCE_EXTMOD}
        ${MICROPY_SOURCE_SHARED}
        ${MICROPY_SOURCE_LIB}
        ${MICROPY_SOURCE_DRIVERS}
        ${MICROPY_SOURCE_PORT}
        ${MICROPY_SOURCE_BOARD}
        xz_micropython.c
        xz_main.c
        xz_stubs.c
        xz_xiaozhi.c
        xz_display.c
        xz_wifi.c
        xz_log.c
        drivers/qmi8658.c
        drivers/pcf85063.c
    INCLUDE_DIRS
        ${MICROPY_INC_CORE}
        ${MICROPY_INC_USERMOD}
        ${MICROPY_PORT_DIR}
        ${MICROPY_BOARD_DIR}
        ${MICROPY_XZ_INCLUDE_DIRS}
        ${CMAKE_BINARY_DIR}
        .
    REQUIRES
        ${MICROPY_XZ_IDF_COMPONENTS}
)

# Set the MicroPython target as the current IDF component target.
set(MICROPY_TARGET ${COMPONENT_TARGET})

# Compile definitions for the trimmed build (littlefs only, no FAT).
# IDF v6 renamed ADC_ATTEN_DB_11 to ADC_ATTEN_DB_12 (same 11dB setting);
# upstream ports/esp32/machine_adc.c still uses the old name. A compile
# definition keeps the shim header-free (board.h must stay include-clean).
# With the PMP IDRAM split, esp_heap_caps.h no longer defines
# MALLOC_CAP_EXEC; modesp32.c exposes it as esp32.HEAP_EXEC, so provide a
# harmless fallback value (the native emitters are unavailable anyway).
if($ENV{IDF_VERSION} VERSION_GREATER_EQUAL "6")
    list(APPEND MICROPY_XZ_COMPILE_DEFS ADC_ATTEN_DB_11=ADC_ATTEN_DB_12)
    if(CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT)
        list(APPEND MICROPY_XZ_COMPILE_DEFS MALLOC_CAP_EXEC=MALLOC_CAP_DEFAULT)
    endif()
endif()

target_compile_definitions(${MICROPY_TARGET} PUBLIC
    ${MICROPY_DEF_COMPONENT}
    ${MICROPY_DEF_CORE}
    ${MICROPY_DEF_BOARD}
    ${MICROPY_XZ_COMPILE_DEFS}
    MICROPY_VFS_LFS2=1
    # modos.c includes oofatfs/ff.h (OS_SYNC); point it at the bundled
    # oofatfs ffconf.h so the search cannot pick up the IDF fatfs one.
    FFCONF_H=\"${MICROPY_OOFATFS_DIR}/ffconf.h\"
    LFS2_NO_MALLOC LFS2_NO_DEBUG LFS2_NO_WARN LFS2_NO_ERROR LFS2_NO_ASSERT
)

# Disable some warnings to keep the build output clean (upstream set).
target_compile_options(${MICROPY_TARGET} PUBLIC
    ${MICROPY_COMPILE_COMPONENT}
    -Wno-clobbered
    -Wno-deprecated-declarations
    -Wno-missing-field-initializers
)

# User C modules don't pick up certain compile options set by the IDF, most
# importantly the optimisation level. So set them here.
idf_build_get_property(idf_compile_options COMPILE_OPTIONS)
target_compile_options(usermod INTERFACE ${idf_compile_options})

target_link_libraries(${MICROPY_TARGET} usermod)

# Upstream panics are routed through MicroPython's panic handler for nicer
# REPL diagnostics (implemented in ports/esp32/panichandler.c). The upstream
# memp_malloc/esp_efuse wrap hooks belong to the socket memory accounting
# and QEMU workarounds and are not needed in this networking-free build
# (they would be global and break lwip/esp_hw_support linking).
target_link_options(${MICROPY_TARGET} PUBLIC
  -Wl,--undefined=esp_panic_handler
  -Wl,--wrap=esp_panic_handler
)

# Collect all of the include directories and compile definitions for the IDF
# components, including those added by the IDF Component Manager.
foreach(comp ${__COMPONENT_NAMES_RESOLVED})
    micropy_gather_target_properties(__idf_${comp})
    micropy_gather_target_properties(${comp})
endforeach()

# Explicitly add extra definitions for MicroPython's preprocessing stage
# (these are not picked up by the above micropy_gather_target_properties).
list(APPEND MICROPY_CPP_DEF_EXTRA "ESP_PLATFORM")

# Include the main MicroPython cmake rules (qstr/genhdr generation).
include(${MICROPY_DIR}/py/mkrules.cmake)

# Generate source files for named pins (requires mkrules.cmake for
# MICROPY_GENHDR_DIR). boards/xiaozhi has no pins.csv, so only the SoC pin
# names are generated.
set(GEN_PINS_PREFIX "${MICROPY_PORT_DIR}/boards/pins_prefix.c")
set(GEN_PINS_MKPINS "${MICROPY_PORT_DIR}/boards/make-pins.py")
set(GEN_PINS_SRC "${CMAKE_BINARY_DIR}/pins.c")
set(GEN_PINS_HDR "${MICROPY_GENHDR_DIR}/pins.h")

target_sources(${MICROPY_TARGET} PRIVATE ${GEN_PINS_HDR})

add_custom_command(
    OUTPUT ${GEN_PINS_SRC} ${GEN_PINS_HDR}
    COMMAND ${Python3_EXECUTABLE} ${GEN_PINS_MKPINS}
        --prefix ${GEN_PINS_PREFIX}
        --output-source ${GEN_PINS_SRC}
        --output-header ${GEN_PINS_HDR}
    DEPENDS
        ${MICROPY_MPVERSION}
        ${GEN_PINS_MKPINS}
        ${GEN_PINS_PREFIX}
    VERBATIM
    COMMAND_EXPAND_LISTS
)
