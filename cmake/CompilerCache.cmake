# SPDX-License-Identifier: GPL-3.0-only
option(CPU12_USE_CCACHE "Use ccache when available" ON)

if(CPU12_USE_CCACHE)
  find_program(CPU12_CCACHE_EXECUTABLE NAMES ccache)
  if(CPU12_CCACHE_EXECUTABLE)
    foreach(language C CXX)
      if(NOT DEFINED CMAKE_${language}_COMPILER_LAUNCHER)
        set(CMAKE_${language}_COMPILER_LAUNCHER "${CPU12_CCACHE_EXECUTABLE}")
      endif()
    endforeach()
  endif()
endif()
