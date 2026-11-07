# Haiku override of cmake/finders/Findqrcodegencpp.cmake
#
# HaikuPorts has no qrcodegencpp package. Use a system copy if one is installed, otherwise build the bundled
# copy in deps/qrcodegen and expose it as the same qrcodegencpp::qrcodegencpp target obs-websocket expects.

# Run the upstream finder (searches the system) without letting it fail the configure step
set(_qrcodegencpp_required ${qrcodegencpp_FIND_REQUIRED})
set(_qrcodegencpp_quiet ${qrcodegencpp_FIND_QUIETLY})
set(qrcodegencpp_FIND_REQUIRED FALSE)
set(qrcodegencpp_FIND_QUIETLY TRUE)
include("${CMAKE_SOURCE_DIR}/cmake/finders/Findqrcodegencpp.cmake")
set(qrcodegencpp_FIND_REQUIRED ${_qrcodegencpp_required})
set(qrcodegencpp_FIND_QUIETLY ${_qrcodegencpp_quiet})
unset(_qrcodegencpp_required)
unset(_qrcodegencpp_quiet)

if(NOT qrcodegencpp_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/deps/qrcodegen/qrcodegen.cpp")
  if(NOT TARGET qrcodegencpp-bundled)
    add_subdirectory("${CMAKE_SOURCE_DIR}/deps/qrcodegen" "${CMAKE_BINARY_DIR}/deps/qrcodegen")
  endif()
  if(NOT TARGET qrcodegencpp::qrcodegencpp)
    add_library(qrcodegencpp::qrcodegencpp ALIAS qrcodegencpp-bundled)
  endif()

  set(qrcodegencpp_FOUND TRUE)
  set(qrcodegencpp_VERSION 1.8.0)
  message(STATUS "Using bundled qrcodegencpp ${qrcodegencpp_VERSION} (deps/qrcodegen)")
elseif(NOT qrcodegencpp_FOUND AND qrcodegencpp_FIND_REQUIRED)
  message(FATAL_ERROR "qrcodegencpp not found and deps/qrcodegen is missing")
endif()
