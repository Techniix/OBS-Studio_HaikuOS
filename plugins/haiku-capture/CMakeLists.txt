cmake_minimum_required(VERSION 3.28...3.30)

add_library(haiku-capture MODULE)
add_library(OBS::haiku-capture ALIAS haiku-capture)

target_sources(haiku-capture PRIVATE haiku-capture.cpp screen-capture.cpp)

target_link_libraries(haiku-capture PRIVATE OBS::libobs be)

set_target_properties_obs(haiku-capture PROPERTIES FOLDER plugins PREFIX "")
