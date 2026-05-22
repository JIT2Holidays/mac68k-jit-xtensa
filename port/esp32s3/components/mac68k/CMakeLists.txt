# mac68k component: the portable emulator core + the Xtensa JIT, built for
# the ESP32-S3. jit/xtensa_sim.c is intentionally excluded — on the target
# the generated code runs natively, so the host simulator is dead weight.

get_filename_component(MAC68K_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

idf_component_register(
    SRCS
        "${MAC68K_ROOT}/core/m68k_interp.c"
        "${MAC68K_ROOT}/core/mac_mem.c"
        "${MAC68K_ROOT}/core/mac_iwm.c"
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

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Populate the dispatcher's stats counters (app_main prints them).
target_compile_definitions(${COMPONENT_LIB} PRIVATE DEBUG=1)

# The IRAM-resident codecache arena. 32 KB comfortably holds the demo's
# working set (~15 blocks); override with -DMAC68K_JIT_ARENA_KB=<n>.
if(NOT DEFINED MAC68K_JIT_ARENA_KB)
    set(MAC68K_JIT_ARENA_KB 32)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE M68K_JIT_ARENA_KB=${MAC68K_JIT_ARENA_KB}u)
