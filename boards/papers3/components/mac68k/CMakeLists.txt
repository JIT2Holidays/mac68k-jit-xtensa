# mac68k component for the PaperS3: the portable emulator core + the Xtensa
# JIT, built for the ESP32-S3. Unlike the qemu demo port, this links the
# FULL Mac (mac_snd, mac_scsi) so the ROM boots a real System volume.
#
# jit/xtensa_sim.c is excluded — on the target the generated code runs
# natively, so the host simulator is dead weight.

get_filename_component(MAC68K_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

idf_component_register(
    SRCS
        "${MAC68K_ROOT}/core/m68k_interp.c"
        "${MAC68K_ROOT}/core/mac_mem.c"
        "${MAC68K_ROOT}/core/mac_iwm.c"
        "${MAC68K_ROOT}/core/mac_input.c"
        "${MAC68K_ROOT}/core/mac_snd.c"
        "${MAC68K_ROOT}/core/mac_scsi.c"
        "${MAC68K_ROOT}/core/sony.c"
        "${MAC68K_ROOT}/core/demo_rom.c"
        "${MAC68K_ROOT}/jit/emit_xtensa.c"
        "${MAC68K_ROOT}/jit/codecache.c"
        "${MAC68K_ROOT}/jit/codegen.c"
        "${MAC68K_ROOT}/jit/dispatcher.c"
        "${CMAKE_CURRENT_LIST_DIR}/mac68k_target.c"
        "${CMAKE_CURRENT_LIST_DIR}/jit_trampolines.S"
    INCLUDE_DIRS
        "${MAC68K_ROOT}/include"
        "${MAC68K_ROOT}/core"
        "${MAC68K_ROOT}/jit"
    REQUIRES esp_common esp_system heap esp_timer
)

# The core/jit sources are developed and warning-validated under the host
# build's flag set, not IDF's stricter -Werror + newer GCC. Keep their
# warnings non-fatal here so a benign upstream diagnostic (e.g. GCC 15's
# -Wmisleading-indentation in mac_input.c) doesn't fail the board build,
# without editing the portable sources.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-error)

# Populate the dispatcher's stats counters (app_main reads arena_cap etc.).
target_compile_definitions(${COMPONENT_LIB} PRIVATE DEBUG=1)

# Compile-time default for m68k_dispatcher_init(); app_main uses
# m68k_dispatcher_init_ex() with BOARD_JIT_ARENA_KB so this is just a floor.
if(NOT DEFINED MAC68K_JIT_ARENA_KB)
    set(MAC68K_JIT_ARENA_KB 96)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE M68K_JIT_ARENA_KB=${MAC68K_JIT_ARENA_KB}u)
