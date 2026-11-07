cmake_minimum_required(VERSION 3.28...3.30)

add_library(haiku-media MODULE)
add_library(OBS::haiku-media ALIAS haiku-media)

target_sources(
  haiku-media
  PRIVATE audio-input.cpp haiku-media.cpp media-device.cpp media-device.hpp video-input.cpp
)

# libmedia: Media Kit (BMediaRoster, BMediaRecorder); libbe: support kit
target_link_libraries(haiku-media PRIVATE OBS::libobs media be)

set_target_properties_obs(haiku-media PROPERTIES FOLDER plugins PREFIX "")
