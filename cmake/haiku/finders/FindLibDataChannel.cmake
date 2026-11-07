# Haiku wrapper around libdatachannel's own CMake package (obs-webrtc calls find_package(LibDataChannel)).
#
# HaikuPorts' libdatachannel_devel installs a LibDataChannelConfig.cmake whose imported target lists
# /boot/system/include as an include directory. Haiku keeps headers in /boot/system/develop/headers and that
# directory doesn't exist, which makes CMake's generate step fail. Load the package normally, then replace
# include directories that don't exist with the directory that actually holds rtc/rtc.hpp.

set(_ldc_args)
if(LibDataChannel_FIND_VERSION)
  list(APPEND _ldc_args ${LibDataChannel_FIND_VERSION})
endif()
if(LibDataChannel_FIND_QUIETLY)
  list(APPEND _ldc_args QUIET)
endif()
if(LibDataChannel_FIND_REQUIRED)
  list(APPEND _ldc_args REQUIRED)
endif()

find_package(LibDataChannel ${_ldc_args} CONFIG)
unset(_ldc_args)

if(LibDataChannel_FOUND)
  find_path(LibDataChannel_HAIKU_INCLUDE_DIR NAMES rtc/rtc.hpp PATHS /boot/system/develop/headers)

  foreach(_ldc_target IN ITEMS LibDataChannel::LibDataChannel LibDataChannel::LibDataChannelStatic)
    if(NOT TARGET ${_ldc_target})
      continue()
    endif()
    get_target_property(_ldc_aliased ${_ldc_target} ALIASED_TARGET)
    if(_ldc_aliased)
      set(_ldc_real ${_ldc_aliased})
    else()
      set(_ldc_real ${_ldc_target})
    endif()

    get_target_property(_ldc_dirs ${_ldc_real} INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT _ldc_dirs)
      continue()
    endif()

    set(_ldc_fixed)
    set(_ldc_changed FALSE)
    foreach(_ldc_dir IN LISTS _ldc_dirs)
      if(_ldc_dir MATCHES "^\\$<" OR EXISTS "${_ldc_dir}")
        list(APPEND _ldc_fixed "${_ldc_dir}")
      else()
        set(_ldc_changed TRUE)
      endif()
    endforeach()

    if(_ldc_changed)
      if(LibDataChannel_HAIKU_INCLUDE_DIR)
        list(APPEND _ldc_fixed "${LibDataChannel_HAIKU_INCLUDE_DIR}")
      endif()
      list(REMOVE_DUPLICATES _ldc_fixed)
      set_property(TARGET ${_ldc_real} PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_ldc_fixed}")
      message(STATUS "Fixed include directories of ${_ldc_target}: ${_ldc_fixed}")
    endif()
  endforeach()

  unset(_ldc_target)
  unset(_ldc_aliased)
  unset(_ldc_real)
  unset(_ldc_dirs)
  unset(_ldc_fixed)
  unset(_ldc_changed)
  unset(_ldc_dir)
endif()
