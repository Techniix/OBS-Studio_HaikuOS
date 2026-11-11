cmake_minimum_required(VERSION 3.28...3.30)

add_library(haiku-video-accel MODULE)
add_library(OBS::haiku-video-accel ALIAS haiku-video-accel)

# Renga's hardware video interface (roadmap F-31 phase 5): BPrivate::VideoAcceleratorRoster and the
# VideoAccelerator/VideoAccelEncoder classes in VideoAcceleration.h, private Media Kit API implemented in libmedia
# (src/kits/media/VideoAcceleration.cpp). haiku_devel ships the header.
find_path(
  RengaMediaPrivate_INCLUDE_DIR
  NAMES VideoAcceleration.h
  PATHS /boot/system/develop/headers/private/media
  NO_DEFAULT_PATH
)
if(NOT RengaMediaPrivate_INCLUDE_DIR)
  message(
    FATAL_ERROR
    "haiku-video-accel needs Renga's video acceleration interface (VideoAcceleration.h in "
    "/boot/system/develop/headers/private/media). This branch builds on Renga only."
  )
endif()

target_sources(
  haiku-video-accel
  PRIVATE haiku-video-accel.cpp va-bitstream.cpp va-bitstream.hpp va-encoder.cpp va-encoder.hpp
)

target_include_directories(haiku-video-accel SYSTEM PRIVATE "${RengaMediaPrivate_INCLUDE_DIR}")

# libmedia: VideoAcceleratorRoster; libbe: BString (VideoAcceleratorRoster::Choice())
target_link_libraries(haiku-video-accel PRIVATE OBS::libobs media be)
target_compile_features(haiku-video-accel PRIVATE cxx_std_17)

set_target_properties_obs(haiku-video-accel PROPERTIES FOLDER plugins PREFIX "")
