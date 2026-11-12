cmake_minimum_required(VERSION 3.28...3.30)

add_library(haiku-media MODULE)
add_library(OBS::haiku-media ALIAS haiku-media)

# Renga's private Media Kit headers (haiku_devel): AudioStreamControl.h, the header-only wrapper around the
# system mixer's per-application streams and its loopback flavor (3rdparty/ce/PORTING.md, "Per-application
# audio, routing and loopback"), and VirtualCamera.h, of which only the constants naming the Virtual Camera are
# used (no feeder code, so neither libgnu nor libnetwork is needed). Only public Media Kit API is called, so
# nothing but libmedia is linked.
find_path(
  RengaMediaPrivate_INCLUDE_DIR
  NAMES AudioStreamControl.h
  PATHS /boot/system/develop/headers/private/media
  NO_DEFAULT_PATH
  REQUIRED
)

target_sources(
  haiku-media
  PRIVATE
    audio-channels.hpp
    audio-convert.hpp
    audio-input.cpp
    haiku-media.cpp
    loopback-audio.cpp
    media-device.cpp
    media-device.hpp
    video-input.cpp
)

target_include_directories(haiku-media SYSTEM PRIVATE "${RengaMediaPrivate_INCLUDE_DIR}")

# libmedia: Media Kit (BMediaRoster, BMediaRecorder); libbe: support and application kits
target_link_libraries(haiku-media PRIVATE OBS::libobs media be)

set_target_properties_obs(haiku-media PROPERTIES FOLDER plugins PREFIX "")
