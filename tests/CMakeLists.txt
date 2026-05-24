add_executable(test_interp test_interp.c)
target_link_libraries(test_interp PRIVATE mac68k_jit)
add_test(NAME interp COMMAND test_interp)

add_executable(test_encoder test_encoder.c)
target_link_libraries(test_encoder PRIVATE mac68k_jit)
add_test(NAME encoder COMMAND test_encoder)

add_executable(test_jit test_jit.c)
target_link_libraries(test_jit PRIVATE mac68k_jit)
add_test(NAME jit_differential COMMAND test_jit)

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
endif()
