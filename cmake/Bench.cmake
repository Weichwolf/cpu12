# SPDX-License-Identifier: GPL-3.0-only
set(CPU12_BENCH_FILTER "" CACHE STRING "Benchmark scenario regex; empty selects all")
set(bench_directory "${CMAKE_CURRENT_BINARY_DIR}/bench")
file(MAKE_DIRECTORY "${bench_directory}")
set(bench_tests "# Generated independent CPU12 benchmark scenarios.\n")
set(bench_targets)
foreach(name kernel word-checksum mandelbrot fft)
  if(NOT TARGET cpu12-${name}-bench)
    continue()
  endif()
  list(APPEND bench_targets cpu12-${name}-bench)
  string(APPEND bench_tests
    "add_test([==[${name}]==] [==[$<TARGET_FILE:cpu12-${name}-bench>]==])\n"
    "set_tests_properties([==[${name}]==] PROPERTIES TIMEOUT 30)\n")
endforeach()
set(packet_policy report)
if(CPU12_SANITIZER STREQUAL "none")
  set(packet_policy enforce)
endif()
foreach(pair "checksum|word-checksum" "fft|fft-stream")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 target)
  list(GET parts 1 scenario)
  if(NOT TARGET cpu12-continuous-${target}-bench)
    continue()
  endif()
  list(APPEND bench_targets cpu12-continuous-${target}-bench)
  string(APPEND bench_tests
    "add_test([==[packet-${scenario}]==] [==[$<TARGET_FILE:cpu12-continuous-${target}-bench>]==] [==[${packet_policy}]==])\n"
    "set_tests_properties([==[packet-${scenario}]==] PROPERTIES TIMEOUT 30)\n")
endforeach()
file(GENERATE OUTPUT "${bench_directory}/CTestTestfile.cmake" CONTENT "${bench_tests}")
set(bench_selection)
if(NOT CPU12_BENCH_FILTER STREQUAL "")
  list(APPEND bench_selection -R "${CPU12_BENCH_FILTER}")
endif()
add_custom_target(${CPU12_TOOL_PREFIX}bench
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${bench_directory}"
    --parallel 1 --no-tests=error --verbose ${bench_selection}
  DEPENDS ${bench_targets}
  USES_TERMINAL
  VERBATIM)
