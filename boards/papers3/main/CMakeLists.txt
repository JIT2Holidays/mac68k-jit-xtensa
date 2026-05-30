# Repo root = this file's dir up three levels (main -> papers3 -> boards -> repo).
get_filename_component(MAC68K_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

idf_component_register(
    SRCS
        "app_main.c"
        "sd_disk.c"
        "uart_ctl.c"
        "eink.c"
        "epd_panel.c"
        "battery.c"
        "touch.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        mac68k
        esp_timer
        fatfs
        sdmmc
        esp_driver_sdspi
        esp_driver_spi
        esp_driver_usb_serial_jtag
        esp_driver_gpio
        # Battery voltage (ADC1) + charge-status GPIO (battery.c).
        esp_adc
        # GT911 capacitive touch -> Mac mouse (touch.c).
        esp_driver_i2c
        # E-ink panel: LCD i80 bus driver + bit-banged gate (epd_panel.c).
        esp_lcd
        # JPEG splash decoder (managed component, see idf_component.yml).
        esp_jpeg
    # Bake the 128 KB Mac Plus ROM into the firmware image so it loads from
    # internal flash (no SD read, always present). Exposed as the symbols
    # _binary_MacPlus_ROM_start / _binary_MacPlus_ROM_end.
    EMBED_FILES "${MAC68K_ROOT}/roms/MacPlus.ROM"
)

# Forward BOARD_* CMake cache vars (set via `idf.py -DBOARD_FOO=bar`) into C
# compile definitions, so the documented build-time overrides actually reach
# the source. Each is optional; unset vars fall back to the #ifndef defaults
# in app_main.c / board_papers3.h.
foreach(opt
        BOARD_USE_JIT BOARD_DEBUG BOARD_QEMU_SELFTEST BOARD_LOCKSTEP BOARD_RAM_MB
        BOARD_JIT_ARENA_KB BOARD_JIT_EVICT BOARD_JIT_HOT_THRESHOLD
        BOARD_HD_INSERT_CYCLE BOARD_EINK_ROT_CW BOARD_EINK_DRIVE_FIELDS BOARD_EINK_MAX_FPS
        BOARD_TOUCH_FLIP_X BOARD_TOUCH_FLIP_Y BOARD_TOUCH_SENS_X10
        BOARD_SD_CLK BOARD_SD_MOSI BOARD_SD_MISO BOARD_SD_CS)
    if(DEFINED ${opt})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${opt}=${${opt}})
    endif()
endforeach()

# Build the emulator core and firmware at -O3 (overrides the -O2 from
# CONFIG_COMPILER_OPTIMIZATION_PERF; the interpreter is hot, and ESP-IDF's
# kconfig tops out at -O2). Applied per-component so the last -O flag wins.
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
