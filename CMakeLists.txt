# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.25)
project(cpu12_core VERSION 0.1.0 LANGUAGES C CXX)
include(cmake/CompilerCache.cmake)
include(CTest)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
foreach(language C CXX)
  if(NOT CMAKE_${language}_COMPILER_ID STREQUAL "Clang" OR
     NOT CMAKE_${language}_COMPILER_VERSION VERSION_GREATER_EQUAL 19 OR
     NOT CMAKE_${language}_COMPILER_VERSION VERSION_LESS 20)
    message(FATAL_ERROR "Use LLVM 19")
  endif()
endforeach()
include(cmake/Quality.cmake)
get_filename_component(CPU12_LLVM_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(CPU12_CLANG_TIDY NAMES clang-tidy-19 clang-tidy
  HINTS "${CPU12_LLVM_BIN}" REQUIRED)
find_program(CPU12_CLANG_FORMAT NAMES clang-format-19 clang-format
  HINTS "${CPU12_LLVM_BIN}" REQUIRED)
foreach(tool IN ITEMS CPU12_CLANG_TIDY CPU12_CLANG_FORMAT)
  execute_process(COMMAND "${${tool}}" --version OUTPUT_VARIABLE tool_version
    COMMAND_ERROR_IS_FATAL ANY)
  if(NOT tool_version MATCHES "version 19\\.")
    message(FATAL_ERROR "${tool} must be LLVM 19")
  endif()
endforeach()
option(CPU12_BUILD_TESTING "Build CPU12 library tests" ${PROJECT_IS_TOP_LEVEL})
set(CMAKE_CXX_CLANG_TIDY "${CPU12_CLANG_TIDY};--warnings-as-errors=*")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(cpu12_host_sources src/jit/a64/run.cpp)
else()
  set(cpu12_host_sources src/jit/run.cpp)
endif()
add_library(cpu12_core STATIC
  src/core.cpp src/cycles.cpp src/image.cpp src/semantic.cpp src/semantic_artifact.cpp
  src/fetch.cpp src/immutable.cpp
  src/decode.cpp
  src/indexed.cpp
  src/registers.cpp
  src/transfer.cpp
  src/loop.cpp
  src/bits.cpp
  src/move.cpp
  src/jit.cpp
  src/jit/llvm/arithmetic.cpp src/jit/llvm/lift.cpp src/jit/llvm/program.cpp src/jit/llvm/regions.cpp
  src/jit/llvm/grants.cpp
  src/jit/llvm/memory.cpp
  src/jit/llvm/indexed.cpp
  src/jit/llvm/flow.cpp
  src/jit/llvm/events.cpp
  src/jit/llvm/registers.cpp src/jit/llvm/math.cpp
  src/jit/llvm/transfer.cpp
  src/jit/llvm/stack.cpp
  src/jit/image.cpp src/jit/llvm/image.cpp
  src/jit/llvm/artifact.cpp
  src/jit/llvm/abi.cpp src/jit/llvm/module.cpp src/jit/llvm/optimize.cpp
  src/jit/llvm/callbacks.cpp src/jit/llvm/runtime_bitcode.cpp
  src/jit/preparation.cpp
  src/runtime.cpp
  ${cpu12_host_sources})
add_library(cpu12::core ALIAS cpu12_core)
target_include_directories(cpu12_core PUBLIC include PRIVATE src)
target_compile_features(cpu12_core PUBLIC cxx_std_23)
target_link_libraries(cpu12_core PUBLIC cpu12_quality)
include(cmake/Llvm.cmake)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(cpu12_native_option -mcpu=native)
else()
  set(cpu12_native_option -march=native)
endif()
target_compile_options(cpu12_core PUBLIC $<$<CONFIG:Release>:${cpu12_native_option}>)
# Instrument adapter IR with the generated program after fusion.
foreach(part runtime core)
  add_library(cpu12-${part}-bitcode OBJECT src/${part}.cpp)
  target_include_directories(cpu12-${part}-bitcode PRIVATE include src "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(cpu12-${part}-bitcode PRIVATE cpu12_diagnostics)
  target_compile_options(cpu12-${part}-bitcode PRIVATE -O3 -emit-llvm ${cpu12_native_option})
  set_target_properties(cpu12-${part}-bitcode PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
endforeach()
find_program(CPU12_LLVM_EXTRACT NAMES llvm-extract llvm-extract-19
  HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED NO_DEFAULT_PATH)
find_program(CPU12_LLVM_LINK NAMES llvm-link llvm-link-19
  HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED NO_DEFAULT_PATH)
file(GLOB_RECURSE cpu12_identity_files CONFIGURE_DEPENDS
  include/*.hpp src/*.hpp src/*.cpp cmake/*)
list(APPEND cpu12_identity_files "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
set(cpu12_identity_input "")
foreach(source IN LISTS cpu12_identity_files)
  file(SHA256 "${source}" digest)
  file(RELATIVE_PATH name "${CMAKE_CURRENT_SOURCE_DIR}" "${source}")
  string(APPEND cpu12_identity_input "${name}:${digest}\n")
endforeach()
string(SHA256 CPU12_SOURCE_ID "${cpu12_identity_input}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${cpu12_identity_files})
configure_file(cmake/SourceIdentity.hpp.in source_identity.hpp @ONLY)
set(cpu12_runtime_bitcode "${CMAKE_CURRENT_BINARY_DIR}/runtime_bitcode.inc")
set(cpu12_runtime_identity "${CMAKE_CURRENT_BINARY_DIR}/runtime_identity.hpp")
add_custom_command(OUTPUT "${cpu12_runtime_bitcode}" "${cpu12_runtime_identity}"
  COMMAND "${CPU12_LLVM_EXTRACT}" $<TARGET_OBJECTS:cpu12-core-bitcode>
    --func=_ZNK5cpu124Core9registersEv --func=_ZNK5cpu124Core5stateEv
    --func=_ZNK5cpu124Core8progressEv --func=_ZN5cpu124Core13request_yieldEv
    -o "${CMAKE_CURRENT_BINARY_DIR}/core_interfaces.bc"
  COMMAND "${CPU12_LLVM_LINK}" $<TARGET_OBJECTS:cpu12-runtime-bitcode>
    "${CMAKE_CURRENT_BINARY_DIR}/core_interfaces.bc"
    -o "${CMAKE_CURRENT_BINARY_DIR}/runtime.bc"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/runtime.bc"
    "-DOUTPUT=${cpu12_runtime_bitcode}" "-DOUTPUT_IDENTITY=${cpu12_runtime_identity}"
    "-DIDENTITY=${CPU12_SOURCE_ID};${LLVM_PACKAGE_VERSION}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBitcode.cmake"
  DEPENDS cpu12-runtime-bitcode cpu12-core-bitcode $<TARGET_OBJECTS:cpu12-runtime-bitcode>
    $<TARGET_OBJECTS:cpu12-core-bitcode> cmake/EmbedBitcode.cmake
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/core_interfaces.bc" "${CMAKE_CURRENT_BINARY_DIR}/runtime.bc"
  VERBATIM)
target_sources(cpu12_core PRIVATE "${cpu12_runtime_bitcode}" "${cpu12_runtime_identity}")
target_include_directories(cpu12_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(NOT APPLE)
  target_link_options(cpu12_core PUBLIC -fuse-ld=lld)
endif()
file(GLOB_RECURSE CPU12_FORMAT_FILES CONFIGURE_DEPENDS include/*.hpp src/*.hpp src/*.cpp test/*.cpp test/*.hpp)
set(CPU12_TOOL_PREFIX "")
if(NOT PROJECT_IS_TOP_LEVEL)
  set(CPU12_TOOL_PREFIX "cpu12-")
endif()
set(CPU12_FORMAT_TARGET "${CPU12_TOOL_PREFIX}format-check")
add_custom_target(${CPU12_FORMAT_TARGET} COMMAND "${CPU12_CLANG_FORMAT}" --dry-run --Werror ${CPU12_FORMAT_FILES} VERBATIM)
if(BUILD_TESTING AND CPU12_BUILD_TESTING)
  add_executable(cpu12-grants-test test/core/grants_test.cpp)
  target_link_libraries(cpu12-grants-test PRIVATE cpu12::core)
  add_test(NAME native-grant-prefixes COMMAND cpu12-grants-test)
  set_tests_properties(native-grant-prefixes PROPERTIES TIMEOUT 30)
  add_executable(cpu12-mac-test test/isa/mac_test.cpp)
  target_link_libraries(cpu12-mac-test PRIVATE cpu12::core)
  add_test(NAME multiply-accumulate-isa COMMAND cpu12-mac-test)
  set_tests_properties(multiply-accumulate-isa PROPERTIES TIMEOUT 30)
  add_executable(cpu12-kernels test/bench/kernels.cpp test/bench/reference.cpp)
  target_include_directories(cpu12-kernels PRIVATE test/support)
  target_include_directories(cpu12-kernels SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(cpu12-kernels PRIVATE cpu12::core LLVM)
  set_source_files_properties(test/bench/reference.cpp PROPERTIES COMPILE_OPTIONS "-fno-lto")
  add_test(NAME common-kernels COMMAND cpu12-kernels --check)
  set_tests_properties(common-kernels PROPERTIES TIMEOUT 120)
  add_executable(cpu12-snapshot-test test/core/snapshot_test.cpp)
  target_link_libraries(cpu12-snapshot-test PRIVATE cpu12::core)
  add_test(NAME processor-snapshots COMMAND cpu12-snapshot-test)
  set_tests_properties(processor-snapshots PROPERTIES TIMEOUT 30)
  add_executable(cpu12-auto-indexed-test test/isa/auto_indexed_test.cpp)
  target_include_directories(cpu12-auto-indexed-test PRIVATE test/support)
  target_link_libraries(cpu12-auto-indexed-test PRIVATE cpu12::core)
  add_test(NAME auto-indexed-lea-isa COMMAND cpu12-auto-indexed-test)
  set_tests_properties(auto-indexed-lea-isa PROPERTIES TIMEOUT 300)
  add_executable(cpu12-stack-test test/isa/stack_test.cpp test/isa/stack_bus.cpp)
  target_include_directories(cpu12-stack-test PRIVATE test/support)
  target_link_libraries(cpu12-stack-test PRIVATE cpu12::core)
  add_test(NAME stack-values-isa COMMAND cpu12-stack-test)
  set_tests_properties(stack-values-isa PROPERTIES TIMEOUT 120)
  add_executable(cpu12-masked-read-test test/isa/masked_read_test.cpp test/isa/masked_read_bus.cpp)
  target_include_directories(cpu12-masked-read-test PRIVATE test/support)
  target_link_libraries(cpu12-masked-read-test PRIVATE cpu12::core)
  add_test(NAME selected-bit-reads-isa COMMAND cpu12-masked-read-test)
  set_tests_properties(selected-bit-reads-isa PROPERTIES TIMEOUT 120)
  add_executable(cpu12-alu-test test/isa/alu_test.cpp)
  target_include_directories(cpu12-alu-test PRIVATE test/support)
  target_link_libraries(cpu12-alu-test PRIVATE cpu12::core)
  add_test(NAME unary-shift-flags-isa COMMAND cpu12-alu-test)
  set_tests_properties(unary-shift-flags-isa PROPERTIES TIMEOUT 120)
  add_executable(cpu12-transfer-test test/isa/transfer_test.cpp)
  target_include_directories(cpu12-transfer-test PRIVATE test/support)
  target_link_libraries(cpu12-transfer-test PRIVATE cpu12::core)
  add_test(NAME transfer-exchange-isa COMMAND cpu12-transfer-test)
  set_tests_properties(transfer-exchange-isa PROPERTIES TIMEOUT 120)
  add_executable(cpu12-accumulator-test test/isa/accumulator_test.cpp test/isa/decimal_test.cpp)
  target_include_directories(cpu12-accumulator-test PRIVATE test/support)
  target_link_libraries(cpu12-accumulator-test PRIVATE cpu12::core)
  add_test(NAME accumulator-decimal-isa COMMAND cpu12-accumulator-test)
  set_tests_properties(accumulator-decimal-isa PROPERTIES TIMEOUT 120)
  add_executable(cpu12-multiply-divide-test test/isa/multiply_test.cpp test/isa/divide_test.cpp)
  target_include_directories(cpu12-multiply-divide-test PRIVATE test/support)
  target_link_libraries(cpu12-multiply-divide-test PRIVATE cpu12::core)
  add_test(NAME multiply-divide-isa COMMAND cpu12-multiply-divide-test)
  set_tests_properties(multiply-divide-isa PROPERTIES TIMEOUT 120)
  add_executable(cpu12-preparation-alu-test test/isa/immediate_test.cpp)
  target_include_directories(cpu12-preparation-alu-test PRIVATE test/support)
  target_link_libraries(cpu12-preparation-alu-test PRIVATE cpu12::core)
  add_test(NAME optimized-immediate-alu COMMAND cpu12-preparation-alu-test)
  set_tests_properties(optimized-immediate-alu PROPERTIES TIMEOUT 30)
  add_library(cpu12-image-bindings OBJECT test/jit/image_bindings.cpp)
  target_include_directories(cpu12-image-bindings PRIVATE include src)
  # Semantic IR receives native instrumentation after fusion in Program.
  target_link_libraries(cpu12-image-bindings PRIVATE cpu12_diagnostics)
  target_compile_options(cpu12-image-bindings PRIVATE -emit-llvm ${cpu12_native_option})
  set_target_properties(cpu12-image-bindings PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
  add_library(cpu12-public-bindings OBJECT test/jit/public_io_bindings.cpp)
  target_include_directories(cpu12-public-bindings PRIVATE include test/support)
  target_link_libraries(cpu12-public-bindings PRIVATE cpu12_diagnostics)
  target_compile_options(cpu12-public-bindings PRIVATE -emit-llvm ${cpu12_native_option})
  set_target_properties(cpu12-public-bindings PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
  add_executable(cpu12-image-test test/jit/image_test.cpp test/jit/image_contract_test.cpp
    test/jit/image_artifact_test.cpp test/jit/image_semantic_test.cpp
    test/jit/image_callbacks_test.cpp test/jit/image_callbacks_events.cpp
    test/jit/image_callbacks_masked.cpp test/jit/public_io_bindings.cpp)
  target_include_directories(cpu12-image-test PRIVATE src test/support)
  target_include_directories(cpu12-image-test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(cpu12-image-test PRIVATE cpu12::core LLVM)
  target_compile_definitions(cpu12-image-test PRIVATE
    CPU12_IMAGE_BINDINGS="$<TARGET_OBJECTS:cpu12-image-bindings>"
    CPU12_PUBLIC_BINDINGS="$<TARGET_OBJECTS:cpu12-public-bindings>")
  add_dependencies(cpu12-image-test cpu12-image-bindings cpu12-public-bindings)
  add_test(NAME complete-code-images COMMAND cpu12-image-test)
  set_tests_properties(complete-code-images PROPERTIES TIMEOUT 30)
  add_executable(cpu12-semantic-image-test test/core/semantic_test.cpp test/core/semantic_native.cpp
    test/core/semantic_calls.cpp test/core/semantic_callbacks.cpp test/core/semantic_artifact.cpp)
  target_include_directories(cpu12-semantic-image-test PRIVATE test/support)
  target_include_directories(cpu12-semantic-image-test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(cpu12-semantic-image-test PRIVATE cpu12::core LLVM ${CMAKE_DL_LIBS})
  target_compile_definitions(cpu12-semantic-image-test PRIVATE
    CPU12_PUBLIC_BINDINGS="$<TARGET_OBJECTS:cpu12-public-bindings>"
    CPU12_RUNTIME_BITCODE="${CMAKE_CURRENT_BINARY_DIR}/runtime.bc")
  add_dependencies(cpu12-semantic-image-test cpu12-public-bindings)
  add_test(NAME public-semantic-images COMMAND cpu12-semantic-image-test)
  set_tests_properties(public-semantic-images PROPERTIES TIMEOUT 30)
  add_executable(cpu12-installed-image-test test/core/image_test.cpp
    test/core/image_contract_test.cpp test/core/image_events_test.cpp test/core/image_deadlines_test.cpp
    test/core/image_io_test.cpp test/core/image_irq_bus.cpp)
  target_include_directories(cpu12-installed-image-test PRIVATE test/support)
  target_link_libraries(cpu12-installed-image-test PRIVATE cpu12::core)
  add_test(NAME installed-native-images COMMAND cpu12-installed-image-test)
  set_tests_properties(installed-native-images PROPERTIES TIMEOUT 30)
  add_executable(cpu12-image-frames-test test/core/image_frames_test.cpp test/core/image_frames_bus.cpp
    test/core/image_frames_deadlines.cpp)
  target_include_directories(cpu12-image-frames-test PRIVATE test/support)
  target_link_libraries(cpu12-image-frames-test PRIVATE cpu12::core)
  add_test(NAME installed-native-frames COMMAND cpu12-image-frames-test)
  set_tests_properties(installed-native-frames PROPERTIES TIMEOUT 30)
  add_test(NAME installed-dynamic-frames COMMAND cpu12-image-frames-test dynamic)
  set_tests_properties(installed-dynamic-frames PROPERTIES TIMEOUT 30)
  add_executable(cpu12-image-transitions-test test/core/image_transitions_test.cpp
    test/core/image_transitions_contract.cpp test/core/image_transitions_control.cpp
    test/core/image_transitions_state.cpp)
  target_include_directories(cpu12-image-transitions-test PRIVATE test/support)
  target_link_libraries(cpu12-image-transitions-test PRIVATE cpu12::core)
  add_test(NAME installed-image-transitions COMMAND cpu12-image-transitions-test)
  set_tests_properties(installed-image-transitions PROPERTIES TIMEOUT 60)
  add_executable(cpu12-image-memory-test test/core/image_memory_test.cpp)
  target_include_directories(cpu12-image-memory-test PRIVATE test/support)
  target_link_libraries(cpu12-image-memory-test PRIVATE cpu12::core)
  add_test(NAME installed-dynamic-memory COMMAND cpu12-image-memory-test)
  set_tests_properties(installed-dynamic-memory PROPERTIES TIMEOUT 60)
  add_executable(cpu12-image-choices-test test/core/image_choices_test.cpp)
  target_include_directories(cpu12-image-choices-test PRIVATE test/support)
  target_link_libraries(cpu12-image-choices-test PRIVATE cpu12::core)
  add_test(NAME installed-image-choices COMMAND cpu12-image-choices-test)
  set_tests_properties(installed-image-choices PROPERTIES TIMEOUT 60)
  add_test(NAME installed-dynamic-image-choices COMMAND cpu12-image-choices-test dynamic)
  set_tests_properties(installed-dynamic-image-choices PROPERTIES TIMEOUT 60)
  add_executable(cpu12-image-composition-test test/core/image_composition_test.cpp)
  target_include_directories(cpu12-image-composition-test PRIVATE test/support)
  target_link_libraries(cpu12-image-composition-test PRIVATE cpu12::core)
  add_test(NAME installed-image-compositions COMMAND cpu12-image-composition-test)
  set_tests_properties(installed-image-compositions PROPERTIES TIMEOUT 60)
  add_executable(cpu12-interrupt-test test/core/interrupt_test.cpp test/core/interrupt_bus.cpp)
  target_link_libraries(cpu12-interrupt-test PRIVATE cpu12::core)
  add_test(NAME interrupt COMMAND cpu12-interrupt-test)
  get_directory_property(core_test_targets BUILDSYSTEM_TARGETS)
  foreach(core_test_target IN LISTS core_test_targets)
    get_target_property(core_test_sources ${core_test_target} SOURCES)
    if(core_test_sources MATCHES "test/")
      target_include_directories(${core_test_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test/support")
    endif()
  endforeach()
endif()
