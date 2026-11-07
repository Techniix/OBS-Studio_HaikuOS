# Haiku override of cmake/finders/FindLibsrt.cmake
#
# HaikuPorts has no libsrt package. Use a system copy if one is installed, otherwise build the bundled SRT
# (deps/srt, see deps/srt-haiku/CMakeLists.txt) and expose it as Libsrt::Libsrt.

set(_libsrt_required ${Libsrt_FIND_REQUIRED})
set(_libsrt_quiet ${Libsrt_FIND_QUIETLY})
set(Libsrt_FIND_REQUIRED FALSE)
set(Libsrt_FIND_QUIETLY TRUE)
include("${CMAKE_SOURCE_DIR}/cmake/finders/FindLibsrt.cmake")
set(Libsrt_FIND_REQUIRED ${_libsrt_required})
set(Libsrt_FIND_QUIETLY ${_libsrt_quiet})
unset(_libsrt_required)
unset(_libsrt_quiet)

if(NOT Libsrt_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/deps/srt/srtcore/srt.h")
  if(NOT TARGET srt-bundled)
    add_subdirectory("${CMAKE_SOURCE_DIR}/deps/srt-haiku" "${CMAKE_BINARY_DIR}/deps/srt-haiku")
  endif()
  if(NOT TARGET Libsrt::Libsrt)
    add_library(Libsrt::Libsrt ALIAS srt-bundled)
  endif()

  set(Libsrt_FOUND TRUE)
  set(Libsrt_VERSION 1.5.4)
  message(STATUS "Using bundled SRT ${Libsrt_VERSION} (deps/srt)")
elseif(NOT Libsrt_FOUND AND Libsrt_FIND_REQUIRED)
  message(FATAL_ERROR "libsrt not found and the deps/srt submodule is missing")
endif()
