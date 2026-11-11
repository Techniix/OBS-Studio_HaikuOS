cmake_minimum_required(VERSION 3.28...3.30)

# Browser source and browser docks on Renga. CEF does not exist for Haiku, so instead of plugins/obs-browser this
# module renders pages with HaikuWebKit (the engine of WebPositive) in a helper process. The module is called
# "obs-browser" on purpose: the source id ("browser_source") and settings match obs-browser, so scene
# collections are portable, and the frontend's browser docks find the panel implementation the same way they
# find the CEF one.

option(ENABLE_BROWSER "Enable browser source and docks (Haiku: HaikuWebKit, package haikuwebkit_devel)" ON)

if(NOT ENABLE_BROWSER)
  target_disable(obs-browser)
  return()
endif()

find_path(HaikuWebKit_INCLUDE_DIR NAMES WebView.h WebPage.h WebWindow.h PATHS /boot/system/develop/headers)
find_library(HaikuWebKit_LIBRARY NAMES WebKitLegacy PATHS /boot/system/develop/lib)
find_library(JavaScriptCore_LIBRARY NAMES JavaScriptCore PATHS /boot/system/develop/lib)

# HaikuWebKit 1.10's WebView.h/WebPage.h include <UrlContext.h> from Haiku's private network services headers
# (haiku_devel ships them; WebPositive builds with the same directory)
find_path(
  HaikuNetServices_INCLUDE_DIR
  NAMES UrlContext.h
  PATHS /boot/system/develop/headers/private/netservices
  NO_DEFAULT_PATH
)

if(NOT HaikuWebKit_INCLUDE_DIR OR NOT HaikuWebKit_LIBRARY OR NOT JavaScriptCore_LIBRARY)
  message(
    FATAL_ERROR
    "HaikuWebKit not found (needed for browser sources and docks). Install it with 'pkgman install haikuwebkit_devel' or configure with -DENABLE_BROWSER=OFF."
  )
endif()

add_library(obs-browser MODULE)
add_library(OBS::browser ALIAS obs-browser)

target_sources(
  obs-browser
  PRIVATE
    browser-client.cpp
    browser-client.hpp
    browser-input.hpp
    browser-ipc.hpp
    browser-source.cpp
    haiku-browser-plugin.cpp
)

# libbe: BMessage (flattened control messages), BAppFileInfo. libgnu: Renga's epoll, eventfd and memfd sealing
# (3rdparty/ce/PORTING.md), the helper channel (browser-ipc.hpp).
target_link_libraries(obs-browser PRIVATE OBS::libobs be gnu)
target_compile_features(obs-browser PRIVATE cxx_std_17)

# Browser docks: implement obs-browser's QCef panel interface. The interface header comes from the obs-browser
# submodule (only panel/browser-panel.hpp is used).
set(_obs_browser_panel_header "${CMAKE_CURRENT_SOURCE_DIR}/../obs-browser/panel/browser-panel.hpp")
if(ENABLE_FRONTEND AND EXISTS "${_obs_browser_panel_header}")
  find_package(Qt6 REQUIRED Widgets)

  add_library(browser-panels INTERFACE)
  add_library(OBS::browser-panels ALIAS browser-panels)
  target_sources(browser-panels INTERFACE "${_obs_browser_panel_header}")
  target_include_directories(browser-panels INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../obs-browser/panel")
  target_compile_definitions(browser-panels INTERFACE BROWSER_AVAILABLE)

  target_sources(obs-browser PRIVATE browser-panel.cpp)
  target_link_libraries(obs-browser PRIVATE OBS::browser-panels Qt::Widgets)
  set_target_properties(obs-browser PROPERTIES AUTOMOC ON)
  target_enable_feature(obs-browser "Browser docks (HaikuWebKit)")
elseif(ENABLE_FRONTEND)
  message(
    WARNING
    "plugins/obs-browser submodule missing: building the browser source without browser docks "
    "(git submodule update --init plugins/obs-browser)"
  )
endif()

set_target_properties_obs(obs-browser PROPERTIES FOLDER plugins/obs-browser PREFIX "")

# The renderer process, installed next to the obs executable
add_executable(obs-haiku-browser)
target_sources(obs-haiku-browser PRIVATE browser-ipc.hpp helper/browser-helper.cpp helper/webkit-abi.h)
target_include_directories(obs-haiku-browser PRIVATE "${HaikuWebKit_INCLUDE_DIR}")
if(HaikuNetServices_INCLUDE_DIR)
  target_include_directories(obs-haiku-browser PRIVATE "${HaikuNetServices_INCLUDE_DIR}")
endif()
# libgnu: memfd_create() and eventfd (Renga)
target_link_libraries(obs-haiku-browser PRIVATE "${HaikuWebKit_LIBRARY}" "${JavaScriptCore_LIBRARY}" be gnu)
target_compile_features(obs-haiku-browser PRIVATE cxx_std_17)
set_target_properties_obs(obs-haiku-browser PROPERTIES FOLDER plugins/obs-browser)
add_dependencies(obs-browser obs-haiku-browser)
