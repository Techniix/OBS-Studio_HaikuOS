cmake_minimum_required(VERSION 3.28...3.30)

add_library(haiku-capture MODULE)
add_library(OBS::haiku-capture ALIAS haiku-capture)

target_sources(haiku-capture PRIVATE capture-throttle.hpp haiku-capture.cpp haiku-window-info.h screen-capture.cpp window-capture.cpp)

target_link_libraries(haiku-capture PRIVATE OBS::libobs be)

set_target_properties_obs(haiku-capture PROPERTIES FOLDER plugins PREFIX "")
