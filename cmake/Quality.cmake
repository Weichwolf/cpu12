# SPDX-License-Identifier: GPL-3.0-only
# Strict project checks; third-party source keeps its own warning policy.
set(CPU12_SANITIZER "address-undefined" CACHE STRING "none, address-undefined, or thread")
set_property(CACHE CPU12_SANITIZER PROPERTY STRINGS none address-undefined thread)
option(CPU12_FUZZING "Build offline libFuzzer targets" OFF)

include("${CMAKE_CURRENT_LIST_DIR}/Hardening.cmake")
add_library(cpu12_quality INTERFACE)
target_link_libraries(cpu12_quality INTERFACE cpu12_hardening)
target_compile_options(cpu12_quality INTERFACE
  -Wall -Wextra -Wpedantic -Werror -pedantic-errors
  -Wconversion -Wsign-conversion -Wshadow -Wundef
  -Wcast-align -Wcast-qual -Wdouble-promotion -Wnull-dereference
  -Wimplicit-fallthrough -Wnon-virtual-dtor -Wold-style-cast
  -Woverloaded-virtual -Wextra-semi -Wvla -Wswitch-enum -Wswitch-default
  -ffp-contract=off -fno-omit-frame-pointer)
# Setup and host API validation use exceptions; ordinary execution reports explicit faults.
target_link_options(cpu12_quality INTERFACE
  -Wl,--fatal-warnings -Wl,-z,defs)

if(CPU12_SANITIZER STREQUAL "address-undefined")
  set(cpu12_sanitizers "address,undefined,float-divide-by-zero,unsigned-integer-overflow,implicit-conversion")
  set(cpu12_dependency_sanitizers "address,undefined,float-divide-by-zero")
elseif(CPU12_SANITIZER STREQUAL "thread")
  set(cpu12_sanitizers "thread")
  set(cpu12_dependency_sanitizers "thread")
elseif(NOT CPU12_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unsupported CPU12_SANITIZER=${CPU12_SANITIZER}")
endif()
if(DEFINED cpu12_sanitizers)
  target_compile_options(cpu12_quality INTERFACE
    "-fsanitize=${cpu12_sanitizers}" -fno-sanitize-recover=all -g)
  target_link_options(cpu12_quality INTERFACE "-fsanitize=${cpu12_sanitizers}")
endif()
if(CPU12_FUZZING)
  if(NOT CPU12_SANITIZER STREQUAL "address-undefined")
    message(FATAL_ERROR "Fuzzing requires address-undefined sanitizers")
  endif()
  target_compile_options(cpu12_quality INTERFACE -fsanitize=fuzzer-no-link)
endif()
