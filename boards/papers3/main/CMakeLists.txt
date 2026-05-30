# Repo root = this file's dir up three levels (main -> papers3 -> boards -> repo).
get_filename_component(MAC68K_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

idf_component_register(
    SRCS
        "app_main.c"
        "sd_disk.c"
        "uart_ctl.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        mac68k
        esp_timer
        fatfs
        sdmmc
        esp_driver_sdspi
        esp_driver_spi
        esp_driver_usb_serial_jtag
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
        BOARD_USE_JIT BOARD_QEMU_SELFTEST BOARD_LOCKSTEP BOARD_RAM_MB
        BOARD_JIT_ARENA_KB BOARD_JIT_EVICT BOARD_JIT_HOT_THRESHOLD
        BOARD_HD_INSERT_CYCLE
        BOARD_SD_CLK BOARD_SD_MOSI BOARD_SD_MISO BOARD_SD_CS)
    if(DEFINED ${opt})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${opt}=${${opt}})
    endif()
endforeach()
