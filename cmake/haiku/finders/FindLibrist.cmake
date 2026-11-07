# Haiku override of cmake/finders/FindLibrist.cmake
#
# HaikuPorts has no librist package. Use a system copy if one is installed, otherwise build the bundled libRIST
# (deps/librist, see deps/librist-haiku/CMakeLists.txt) and expose it as Librist::Librist.

set(_librist_required ${Librist_FIND_REQUIRED})
set(_librist_quiet ${Librist_FIND_QUIETLY})
set(Librist_FIND_REQUIRED FALSE)
set(Librist_FIND_QUIETLY TRUE)
include("${CMAKE_SOURCE_DIR}/cmake/finders/FindLibrist.cmake")
set(Librist_FIND_REQUIRED ${_librist_required})
set(Librist_FIND_QUIETLY ${_librist_quiet})
unset(_librist_required)
unset(_librist_quiet)

if(NOT Librist_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/deps/librist/src/rist.c")
  if(NOT TARGET librist-bundled)
    add_subdirectory("${CMAKE_SOURCE_DIR}/deps/librist-haiku" "${CMAKE_BINARY_DIR}/deps/librist-haiku")
  endif()
  if(NOT TARGET Librist::Librist)
    add_library(Librist::Librist ALIAS librist-bundled)
  endif()

  set(Librist_FOUND TRUE)
  set(Librist_VERSION 0.2.12)
  message(STATUS "Using bundled libRIST ${Librist_VERSION} (deps/librist)")
elseif(NOT Librist_FOUND AND Librist_FIND_REQUIRED)
  message(FATAL_ERROR "librist not found and the deps/librist submodule is missing")
endif()
