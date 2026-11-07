# OBS CMake Haiku compiler configuration module

include_guard(GLOBAL)

# Upstream builds with warnings-as-errors. Haiku's system headers and GCC produce a different set of
# warnings than the CI-tested platforms, so default to OFF here (pass -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
# to opt back in).
if(NOT DEFINED CMAKE_COMPILE_WARNING_AS_ERROR)
  set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
endif()

# Haiku uses GCC (or Clang) with a POSIX userland, so the Linux compiler flags apply as-is.
include("${CMAKE_SOURCE_DIR}/cmake/linux/compilerconfig.cmake")

# C++ is compiled as strict ISO C++ (CMAKE_CXX_EXTENSIONS is OFF, as on Linux). glibc's g++ predefines
# _GNU_SOURCE so POSIX/BSD extensions stay visible there; Haiku's g++ doesn't, and its headers then hide
# things like NI_MAXHOST, HOST_NOT_FOUND or getifaddrs() (needed by Asio/obs-websocket and others).
add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:_DEFAULT_SOURCE>)
