# OBS CMake Haiku defaults module

include_guard(GLOBAL)

# Linux-only backends that do not exist on Haiku. These are referenced by name throughout the build,
# so define them as plain variables instead of user-facing options.
set(ENABLE_PULSEAUDIO OFF)
set(ENABLE_WAYLAND OFF)

option(ENABLE_RELOCATABLE "Enable relocatable build" OFF)
option(ENABLE_PORTABLE_CONFIG "Enable support for portable config file location" OFF)

# Set default installation directories. On Haiku, packages install into /boot/system (HaikuPorts) while
# manual installs go to /boot/home/config/non-packaged.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/boot/home/config/non-packaged" CACHE PATH "Install prefix" FORCE)
endif()

include(GNUInstallDirs)

if(ENABLE_RELOCATABLE)
  set(OBS_EXECUTABLE_RPATH "$ORIGIN/;$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  set(OBS_LIBRARY_RPATH "$ORIGIN/")
  set(OBS_MODULE_RPATH "$ORIGIN/;$ORIGIN/..")
endif()

set(OBS_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/rundir")
set(OBS_EXECUTABLE_DESTINATION "${CMAKE_INSTALL_BINDIR}")
set(OBS_INCLUDE_DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/obs")
set(OBS_LIBRARY_DESTINATION "${CMAKE_INSTALL_LIBDIR}")
set(OBS_PLUGIN_DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-modules")
set(OBS_SCRIPT_PLUGIN_DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-scripting")
set(OBS_DATA_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs")
set(OBS_CMAKE_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake")

# Set additional paths used by OBS for self-discovery
set(OBS_PLUGIN_PATH "${CMAKE_INSTALL_LIBDIR}/obs-modules/core")
set(OBS_SCRIPT_PLUGIN_PATH "${CMAKE_INSTALL_LIBDIR}/obs-scripting")
set(OBS_DATA_PATH "${OBS_DATA_DESTINATION}")
set(OBS_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Enable find_package targets to become globally available targets
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

# Header-only dependencies bundled as submodules (deps/*), used unless the cache variable is set explicitly:
# - SIMDe isn't packaged in HaikuPorts.
# - obs-websocket needs WebSocket++ 0.8.2 with an Asio that still has io_service (removed in Asio 1.33);
#   HaikuPorts ships Asio 1.38, so a matching pair (Asio 1.30.2 + WebSocket++ 0.8.2) is bundled.
# qrcodegencpp (also needed by obs-websocket) is handled by cmake/haiku/finders/Findqrcodegencpp.cmake.
if(NOT DEFINED SIMDe_INCLUDE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/deps/simde/simde/simde-common.h")
  set(SIMDe_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/deps/simde" CACHE PATH "Directory containing simde/simde-common.h")
endif()
if(NOT DEFINED Asio_INCLUDE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/deps/asio/asio/include/asio.hpp")
  set(Asio_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/deps/asio/asio/include" CACHE PATH "Directory containing asio.hpp")
endif()
if(NOT DEFINED Websocketpp_INCLUDE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/deps/websocketpp/websocketpp/client.hpp")
  set(
    Websocketpp_INCLUDE_DIR
    "${CMAKE_SOURCE_DIR}/deps/websocketpp"
    CACHE PATH
    "Directory containing websocketpp/client.hpp"
  )
endif()
