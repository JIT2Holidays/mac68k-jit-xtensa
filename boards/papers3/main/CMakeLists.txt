# Repo root = this file's dir up three levels (main -> papers3 -> boards -> repo).
get_filename_component(MAC68K_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# E-paper driver. Default = the custom internal-SRAM 1bpp driver (msg_panel.c +
# msg.c), purpose-built for this emulator (21 KB internal-SRAM fb, fast localized
# refresh, ~10 MHz emu during motion). The FastEPD glue (eink_fastepd.cpp) is
# always compiled and FastEPD is always a requirement, so toggling the
# -DBOARD_USE_FASTEPD compile option never changes the component graph; when the
# option is OFF the glue is an empty TU and FastEPD is GC'd from the image.
idf_component_register(
    SRCS
        "app_main.c" "sd_disk.c" "uart_ctl.c"
        "msg.c" "msg_panel.c" "eink_fastepd.cpp"
        "battery.c" "touch.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        mac68k esp_timer fatfs sdmmc
        esp_driver_sdspi esp_driver_spi esp_driver_usb_serial_jtag esp_driver_gpio
        esp_adc            # battery (battery.c)
        esp_driver_i2c     # GT911 touch (touch.c)
        esp_lcd            # e-ink i80 bus (msg_panel.c / FastEPD)
        esp_jpeg           # JPEG splash (managed component)
        FastEPD            # optional driver, -DBOARD_USE_FASTEPD=1 (eink_fastepd.cpp)
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
        BOARD_JIT_PREFETCH BOARD_JIT_PREFETCH_DEPTH BOARD_JIT_COMPILE_BUDGET BOARD_NO_EINK BOARD_NO_TOUCH
        BOARD_HD_INSERT_CYCLE BOARD_EINK_ROT_CW BOARD_EINK_DRIVE_FIELDS BOARD_EINK_MAX_FPS
        BOARD_EINK_HOLD_DRIVE BOARD_EINK_HOLD_STATIC_STOP
        BOARD_USE_FASTEPD
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
