add_executable(test_interp test_interp.c)
target_link_libraries(test_interp PRIVATE mac68k_jit)
add_test(NAME interp COMMAND test_interp)

add_executable(test_encoder test_encoder.c)
target_link_libraries(test_encoder PRIVATE mac68k_jit)
add_test(NAME encoder COMMAND test_encoder)

add_executable(test_jit test_jit.c)
target_link_libraries(test_jit PRIVATE mac68k_jit)
add_test(NAME jit_differential COMMAND test_jit)
