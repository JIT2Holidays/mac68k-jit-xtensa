cmake_minimum_required(VERSION 3.16)
project(mac68k_jit_xtensa C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)

# Host build defines DEBUG so the dispatcher's counters are populated and
# the CLI / tests can read them.
add_compile_definitions(DEBUG=1)

# Portable emulator core: 68000 interpreter + Mac memory/peripherals.
add_library(mac68k_core STATIC
    core/m68k_interp.c
    core/mac_mem.c
    core/mac_iwm.c
    core/sony.c
    core/demo_rom.c
)
target_include_directories(mac68k_core PUBLIC include core)

# JIT codegen: Xtensa LX7 emitter, code cache, codegen, dispatcher, and
# (host only) the in-tree Xtensa simulator that executes JIT output.
add_library(mac68k_jit STATIC
    jit/emit_xtensa.c
    jit/codecache.c
    jit/codegen.c
    jit/dispatcher.c
    jit/xtensa_sim.c
)
target_include_directories(mac68k_jit PUBLIC include core jit)
target_link_libraries(mac68k_jit PUBLIC mac68k_core)

# Host CLI driver.
add_executable(mac68k_host port/host/main.c)
target_link_libraries(mac68k_host PRIVATE mac68k_jit)

enable_testing()
add_subdirectory(tests)
