# CMake generated Testfile for 
# Source directory: /Users/yukidama/github/mac68k-jit-xtensa/tests
# Build directory: /Users/yukidama/github/mac68k-jit-xtensa/build_histo/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(interp "/Users/yukidama/github/mac68k-jit-xtensa/build_histo/tests/test_interp")
set_tests_properties(interp PROPERTIES  _BACKTRACE_TRIPLES "/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;3;add_test;/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;0;")
add_test(encoder "/Users/yukidama/github/mac68k-jit-xtensa/build_histo/tests/test_encoder")
set_tests_properties(encoder PROPERTIES  _BACKTRACE_TRIPLES "/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;7;add_test;/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;0;")
add_test(jit_differential "/Users/yukidama/github/mac68k-jit-xtensa/build_histo/tests/test_jit")
set_tests_properties(jit_differential PROPERTIES  _BACKTRACE_TRIPLES "/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;11;add_test;/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;0;")
add_test(prefetch "/Users/yukidama/github/mac68k-jit-xtensa/build_histo/tests/test_prefetch")
set_tests_properties(prefetch PROPERTIES  _BACKTRACE_TRIPLES "/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;18;add_test;/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;0;")
add_test(diff_jit_bench_lockstep "/Users/yukidama/github/mac68k-jit-xtensa/build_histo/mac68k_host" "--diff-jit-trace" "--no-irq" "--load-snapshot" "/Users/yukidama/github/mac68k-jit-xtensa/roms/disks/speedo-bench.snap" "--max-cycles" "11000")
set_tests_properties(diff_jit_bench_lockstep PROPERTIES  WORKING_DIRECTORY "/Users/yukidama/github/mac68k-jit-xtensa" _BACKTRACE_TRIPLES "/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;27;add_test;/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;0;")
add_test(diff_jit_bench_lockstep_prefetch "/Users/yukidama/github/mac68k-jit-xtensa/build_histo/mac68k_host" "--diff-jit-trace" "--no-irq" "--prefetch" "static" "--load-snapshot" "/Users/yukidama/github/mac68k-jit-xtensa/roms/disks/speedo-bench.snap" "--max-cycles" "11000")
set_tests_properties(diff_jit_bench_lockstep_prefetch PROPERTIES  WORKING_DIRECTORY "/Users/yukidama/github/mac68k-jit-xtensa" _BACKTRACE_TRIPLES "/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;36;add_test;/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;0;")
add_test(diff_jit_bench_lockstep_prefetch_chain "/Users/yukidama/github/mac68k-jit-xtensa/build_histo/mac68k_host" "--diff-jit-trace" "--no-irq" "--prefetch" "chain" "--load-snapshot" "/Users/yukidama/github/mac68k-jit-xtensa/roms/disks/speedo-bench.snap" "--max-cycles" "11000")
set_tests_properties(diff_jit_bench_lockstep_prefetch_chain PROPERTIES  WORKING_DIRECTORY "/Users/yukidama/github/mac68k-jit-xtensa" _BACKTRACE_TRIPLES "/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;45;add_test;/Users/yukidama/github/mac68k-jit-xtensa/tests/CMakeLists.txt;0;")
