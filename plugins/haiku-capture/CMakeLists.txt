cmake_minimum_required(VERSION 3.28...3.30)

add_library(haiku-capture MODULE)
add_library(OBS::haiku-capture ALIAS haiku-capture)

# Renga's capture API is private API of libbe: BPrivate::BScreenCapture (ScreenCapture.h, in private/interface,
# which includes ScreenCaptureDefs.h from private/app), and the window-list API in WindowInfo.h (private/interface).
# haiku_devel ships the headers.
find_path(
  RengaInterfacePrivate_INCLUDE_DIR
  NAMES ScreenCapture.h
  PATHS /boot/system/develop/headers/private/interface
  NO_DEFAULT_PATH
)
find_path(
  RengaAppPrivate_INCLUDE_DIR
  NAMES ScreenCaptureDefs.h
  PATHS /boot/system/develop/headers/private/app
  NO_DEFAULT_PATH
)
if(NOT RengaInterfacePrivate_INCLUDE_DIR OR NOT RengaAppPrivate_INCLUDE_DIR)
  message(
    FATAL_ERROR
    "haiku-capture needs Renga's capture API (ScreenCapture.h and ScreenCaptureDefs.h in "
    "/boot/system/develop/headers/private). This branch builds on Renga only."
  )
endif()

target_sources(
  haiku-capture
  PRIVATE capture-source.cpp capture-source.hpp haiku-capture.cpp screen-capture.cpp window-capture.cpp
)

target_include_directories(
  haiku-capture
  SYSTEM
  PRIVATE "${RengaInterfacePrivate_INCLUDE_DIR}" "${RengaAppPrivate_INCLUDE_DIR}"
)

# libbe: BScreenCapture, BBitmap, BScreen, BLooper (the permission answers), the window list
target_link_libraries(haiku-capture PRIVATE OBS::libobs be)
target_compile_features(haiku-capture PRIVATE cxx_std_17)

set_target_properties_obs(haiku-capture PROPERTIES FOLDER plugins PREFIX "")
