cmake_minimum_required(VERSION 3.28...3.30)

add_library(haiku-virtualcam MODULE)
add_library(OBS::haiku-virtualcam ALIAS haiku-virtualcam)

# Renga's virtual camera (roadmap F-58, request O-12): BPrivate::VirtualCameraFeeder in the header-only private
# Media Kit header VirtualCamera.h (haiku_devel). It feeds the "Virtual Camera" flavor of the virtual_camera media
# add-on through a memfd and a local SOCK_SEQPACKET socket. A cache variable of its own, so that it does not share
# (and depend on the order of) the find_path() results of the other haiku-* plugins.
find_path(
  RengaVirtualCamera_INCLUDE_DIR
  NAMES VirtualCamera.h
  PATHS /boot/system/develop/headers/private/media
  NO_DEFAULT_PATH
)
if(NOT RengaVirtualCamera_INCLUDE_DIR)
  message(
    FATAL_ERROR
    "haiku-virtualcam needs Renga's virtual camera interface (VirtualCamera.h in "
    "/boot/system/develop/headers/private/media). This branch builds on Renga only."
  )
endif()

target_sources(haiku-virtualcam PRIVATE haiku-virtualcam.cpp)

target_include_directories(haiku-virtualcam SYSTEM PRIVATE "${RengaVirtualCamera_INCLUDE_DIR}")

# VirtualCamera.h: libgnu for memfd_create(), libnetwork for the socket calls (as the header and Renga's own
# virtual_camera add-on link). find_paths_etc() and the threads are in libroot.
target_link_libraries(haiku-virtualcam PRIVATE OBS::libobs gnu network)
target_compile_features(haiku-virtualcam PRIVATE cxx_std_17)

set_target_properties_obs(haiku-virtualcam PROPERTIES FOLDER plugins PREFIX "")
