# SPDX-License-Identifier: GPL-3.0-only
set(CPU12_BENCH_FILTER "" CACHE STRING "Benchmark scenario regex; empty selects all")
set(bench_directory "${CMAKE_CURRENT_BINARY_DIR}/bench")
file(MAKE_DIRECTORY "${bench_directory}")
set(bench_tests "# Generated independent CPU12 benchmark scenarios.\n")
foreach(name kernel word-checksum mandelbrot fft)
  string(APPEND bench_tests
    "add_test([==[${name}]==] [==[$<TARGET_FILE:cpu12-${name}-bench>]==])\n"
    "set_tests_properties([==[${name}]==] PROPERTIES TIMEOUT 30)\n")
endforeach()
set(packet_policy report)
if(CPU12_SANITIZER STREQUAL "none")
  set(packet_policy enforce)
endif()
string(APPEND bench_tests
  "add_test([==[packet-word-checksum]==] [==[$<TARGET_FILE:cpu12-continuous-checksum-bench>]==] [==[${packet_policy}]==])\n"
  "set_tests_properties([==[packet-word-checksum]==] PROPERTIES TIMEOUT 30)\n"
  "add_test([==[packet-fft-stream]==] [==[$<TARGET_FILE:cpu12-continuous-fft-bench>]==] [==[${packet_policy}]==])\n"
  "set_tests_properties([==[packet-fft-stream]==] PROPERTIES TIMEOUT 30)\n")
file(GENERATE OUTPUT "${bench_directory}/CTestTestfile.cmake" CONTENT "${bench_tests}")
set(bench_selection)
if(NOT CPU12_BENCH_FILTER STREQUAL "")
  list(APPEND bench_selection -R "${CPU12_BENCH_FILTER}")
endif()
add_custom_target(bench
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${bench_directory}"
    --parallel 1 --no-tests=error --verbose ${bench_selection}
  DEPENDS cpu12-kernel-bench cpu12-word-checksum-bench cpu12-mandelbrot-bench cpu12-fft-bench
    cpu12-continuous-checksum-bench cpu12-continuous-fft-bench
  USES_TERMINAL
  VERBATIM)
