add_executable(test_interp test_interp.c)
target_link_libraries(test_interp PRIVATE mac68k_jit)
add_test(NAME interp COMMAND test_interp)

add_executable(test_encoder test_encoder.c)
target_link_libraries(test_encoder PRIVATE mac68k_jit)
add_test(NAME encoder COMMAND test_encoder)

add_executable(test_jit test_jit.c)
target_link_libraries(test_jit PRIVATE mac68k_jit)
add_test(NAME jit_differential COMMAND test_jit)

# M6.71 — unit tests for the static-successor analysis used by the JIT
# block-prefetch policy. Hand-crafts blocks with every kind of
# terminator and checks the helper returns the right successor PCs.
add_executable(test_prefetch test_prefetch.c)
target_link_libraries(test_prefetch PRIVATE mac68k_jit)
add_test(NAME prefetch COMMAND test_prefetch)

# Block-by-block differential of JIT vs interp on a frozen Speedometer
# snapshot. Runs up to just before the documented VIA-tick granularity
# divergence at cycle 11898 (M6.66/M6.68). Locks in the M6.68/M6.69
# SR-flush fix and catches future regressions of that bug class. The
# snapshot lives outside the repo (large, regenerable); the test is
# conditional on its presence so CI / fresh checkouts don't fail.
if(EXISTS "${CMAKE_SOURCE_DIR}/roms/disks/speedo-bench.snap")
    add_test(NAME diff_jit_bench_lockstep
             COMMAND $<TARGET_FILE:mac68k_host>
                     --diff-jit-trace --no-irq
                     --load-snapshot ${CMAKE_SOURCE_DIR}/roms/disks/speedo-bench.snap
                     --max-cycles 11000
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # M6.71 — same lockstep test but with prefetch enabled. Confirms
    # prefetch doesn't perturb the JIT/interp invariants.
    add_test(NAME diff_jit_bench_lockstep_prefetch
             COMMAND $<TARGET_FILE:mac68k_host>
                     --diff-jit-trace --no-irq --prefetch static
                     --load-snapshot ${CMAKE_SOURCE_DIR}/roms/disks/speedo-bench.snap
                     --max-cycles 11000
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # M6.72 — lockstep with the new chain prefetch policy. Confirms
    # the deeper-but-selective walk doesn't perturb correctness.
    add_test(NAME diff_jit_bench_lockstep_prefetch_chain
             COMMAND $<TARGET_FILE:mac68k_host>
                     --diff-jit-trace --no-irq --prefetch chain
                     --load-snapshot ${CMAKE_SOURCE_DIR}/roms/disks/speedo-bench.snap
                     --max-cycles 11000
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# M6.153 — extra bench snapshots that broaden differential coverage
# beyond Speedometer's tight ALU loop. See STATUS.md §"Future bench
# targets" — the originally proposed MacBench 4.0 / THINK C targets
# remained blocked on the M6.67 Finder app-launch wall, so these two
# boot-phase snapshots stand in for them. Both pass the full 11K
# --diff-jit-trace --no-irq window and exercise distinct code paths:
#
#   * boot-rom-init.snap     pc=0x40032C — boot-ROM memory-test loop,
#                            dominated by BTST.B #imm,(d16,An) (op 082D).
#   * boot-system-load.snap  pc=0x401F6E — post-System-load ROM phase,
#                            ROM trap arg-passing (MOVE.L 2F72 heavy).
#
# Both snapshots are 1.18 MB and gitignored (regenerate via the
# scripts/snap-extra-bench.sh script in this directory).
if(EXISTS "${CMAKE_SOURCE_DIR}/roms/disks/boot-rom-init.snap")
    add_test(NAME diff_jit_boot_rom_init_lockstep
             COMMAND $<TARGET_FILE:mac68k_host>
                     --diff-jit-trace --no-irq
                     --load-snapshot ${CMAKE_SOURCE_DIR}/roms/disks/boot-rom-init.snap
                     --max-cycles 11000
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/roms/disks/boot-system-load.snap")
    add_test(NAME diff_jit_boot_system_load_lockstep
             COMMAND $<TARGET_FILE:mac68k_host>
                     --diff-jit-trace --no-irq
                     --load-snapshot ${CMAKE_SOURCE_DIR}/roms/disks/boot-system-load.snap
                     --max-cycles 11000
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# M6.197 — mid-boot snapshot at cycle 100M (between boot-system-load=60M
# and the thinkc8 Finder-steady-state=1.9B). Captures the INIT/extension
# load phase. Hot opcodes include BCHG D4,(A1), MOVE.L (A0)+,(A0), and
# MOVE.L (A0)+,(A4)+ — distinct mix from the other three snapshots.
# Passes the full 11K --diff-jit-trace --no-irq window (150 blocks).
# Regenerate via scripts/snap-cycle100m.sh.
if(EXISTS "${CMAKE_SOURCE_DIR}/roms/disks/boot-cycle100m.snap")
    add_test(NAME diff_jit_boot_cycle100m_lockstep
             COMMAND $<TARGET_FILE:mac68k_host>
                     --diff-jit-trace --no-irq
                     --load-snapshot ${CMAKE_SOURCE_DIR}/roms/disks/boot-cycle100m.snap
                     --max-cycles 11000
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
