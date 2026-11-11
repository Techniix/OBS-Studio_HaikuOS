include(CheckCSourceCompiles)

set(LIBM_TEST_SOURCE "#include<math.h>\nfloat f; int main(){sqrt(f);return 0;}")
check_c_source_compiles("${LIBM_TEST_SOURCE}" HAVE_MATH_IN_STD_LIB)

target_sources(
  libobs
  PRIVATE
    audio-monitoring/haiku/haiku-output.c
    audio-monitoring/haiku/haiku-sound-player.cpp
    audio-monitoring/haiku/haiku-sound-player.h
    obs-haiku-be.cpp
    obs-haiku.c
    obs-haiku.h
    obs-nix-platform.c
    obs-nix-platform.h
    util/pipe-posix.c
    util/platform-nix.c
    util/threading-posix.c
    util/threading-posix.h
)

target_compile_definitions(
  libobs
  PRIVATE
    OBS_INSTALL_PREFIX="${OBS_INSTALL_PREFIX}"
    $<$<COMPILE_LANG_AND_ID:C,GNU>:ENABLE_DARRAY_TYPE_TEST>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:ENABLE_DARRAY_TYPE_TEST>
)

if(CMAKE_C_COMPILER_ID STREQUAL GNU)
  # * Silence type-limits warning in line 292 of libobs/utils/utf8.c
  if(CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 12.3.0)
    target_compile_options(libobs PRIVATE -Wno-error=type-limits)
  endif()
endif()

# Audio monitoring uses Renga's private, header-only AudioStreamControl.h (haiku_devel) to route OBS's streams in
# the system mixer (3rdparty/ce/PORTING.md, "Per-application audio, routing and loopback").
find_path(
  RengaMediaPrivate_INCLUDE_DIR
  NAMES AudioStreamControl.h
  PATHS /boot/system/develop/headers/private/media
  NO_DEFAULT_PATH
  REQUIRED
)
set_source_files_properties(
  audio-monitoring/haiku/haiku-sound-player.cpp
  PROPERTIES INCLUDE_DIRECTORIES "${RengaMediaPrivate_INCLUDE_DIR}"
)

# libroot provides libc, libm, libdl and pthreads; libbe provides the Interface Kit (keyboard/mouse state);
# libmedia provides BSoundPlayer for audio monitoring; libgame provides set_mouse_position(), used to keep the
# screen saver from starting while recording/streaming (os_inhibit_sleep).
target_link_libraries(libobs PRIVATE be game media $<$<NOT:$<BOOL:${HAVE_MATH_IN_STD_LIB}>>:m>)

target_enable_feature(libobs "Haiku platform support")
target_enable_feature(libobs "Media Kit audio monitoring (Haiku)")

set_target_properties(libobs PROPERTIES OUTPUT_NAME obs)
