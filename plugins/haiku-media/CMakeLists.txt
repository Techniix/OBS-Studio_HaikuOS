cmake_minimum_required(VERSION 3.28...3.30)

add_library(haiku-media MODULE)
add_library(OBS::haiku-media ALIAS haiku-media)

target_sources(
  haiku-media
  PRIVATE
    audio-input.cpp
    audio-tap/audio-tap-shm.h
    desktop-audio.cpp
    haiku-media.cpp
    media-device.cpp
    media-device.hpp
    video-input.cpp
)

# libmedia: Media Kit (BMediaRoster, BMediaRecorder); libbe: support kit
target_link_libraries(haiku-media PRIVATE OBS::libobs media be)

set_target_properties_obs(haiku-media PROPERTIES FOLDER plugins PREFIX "")

# Desktop audio helper: inserts a pass-through node between the system mixer and the sound card and shares the
# mixer output with the plugin through a cloneable area. Installed next to the obs executable, where
# desktop-audio.cpp looks for it.
add_executable(obs-haiku-audio-tap)
target_sources(obs-haiku-audio-tap PRIVATE audio-tap/audio-tap-shm.h audio-tap/tap-app.cpp audio-tap/tap-node.cpp audio-tap/tap-node.h)
target_link_libraries(obs-haiku-audio-tap PRIVATE media be)

set_target_properties_obs(obs-haiku-audio-tap PROPERTIES FOLDER plugins/haiku-media)
add_dependencies(haiku-media obs-haiku-audio-tap)
