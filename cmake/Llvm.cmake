# SPDX-License-Identifier: GPL-3.0-only
find_program(CPU12_LLVM_CONFIG NAMES llvm-config-19 llvm-config
  HINTS "${CPU12_LLVM_BIN}" REQUIRED)
execute_process(COMMAND "${CPU12_LLVM_CONFIG}" --cmakedir
  OUTPUT_VARIABLE cpu12_llvm_cmake OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
find_package(LLVM 19.1 CONFIG REQUIRED GLOBAL PATHS "${cpu12_llvm_cmake}" NO_DEFAULT_PATH)
if(NOT LLVM_VERSION_MAJOR EQUAL 19)
  message(FATAL_ERROR "CPU12 requires packaged LLVM 19 development libraries")
endif()
target_include_directories(cpu12_core SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(cpu12_core PRIVATE LLVM)
