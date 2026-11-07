# Builds the bundled SRT library (deps/srt, Haivision SRT v1.5.4, MPL-2.0) as a static library for Haiku.
#
# SRT's own CMake project rejects unknown systems ("Unsupported system: Haiku") and installs its headers and
# pkg-config files alongside OBS. Its library code is plain POSIX/C++11, so this builds the same sources with
# the configuration SRT would pick for a generic POSIX system:
#   C++11 threads/chrono (ENABLE_STDCXX_SYNC), CLOCK_MONOTONIC, mbedTLS encryption, logging on, no bonding.

include("${CMAKE_SOURCE_DIR}/cmake/haiku/haiku-patch.cmake")

set(SRT_SRC_DIR "${CMAKE_SOURCE_DIR}/deps/srt")
set(SRT_DIR "${CMAKE_CURRENT_BINARY_DIR}/srt-src")

# Haiku patches (applied to a copy in the build tree):
# - utilities.h: Haiku's <endian.h> (headers/bsd) provides the glibc-style htobe64()/le32toh() family.
foreach(_d srtcore haicrypt common)
  haiku_copy_tree("${SRT_SRC_DIR}/${_d}" "${SRT_DIR}/${_d}" EXCLUDE utilities.h)
endforeach()
haiku_patch_file(
  "${SRT_SRC_DIR}/srtcore/utilities.h"
  "${SRT_DIR}/srtcore/utilities.h"
  "#if defined(__linux__) || defined(__CYGWIN__) || defined(__GNU__) || defined(__GLIBC__)"
  "#if defined(__linux__) || defined(__CYGWIN__) || defined(__GNU__) || defined(__GLIBC__) || defined(__HAIKU__)"
)
set(SRT_VERSION_MAJOR 1)
set(SRT_VERSION_MINOR 5)
set(SRT_VERSION_PATCH 4)
set(SRT_VERSION "${SRT_VERSION_MAJOR}.${SRT_VERSION_MINOR}.${SRT_VERSION_PATCH}")

find_package(MbedTLS 3...<4 REQUIRED)
find_package(Threads REQUIRED)

# Public headers as <srt/...>, like an installed SRT
set(_srt_inc "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file("${SRT_DIR}/srtcore/version.h.in" "${_srt_inc}/srt/version.h" @ONLY)
foreach(_h srt.h logging_api.h access_control.h platform_sys.h udt.h)
  configure_file("${SRT_DIR}/srtcore/${_h}" "${_srt_inc}/srt/${_h}" COPYONLY)
endforeach()

add_library(
  srt-bundled
  STATIC
  ${SRT_DIR}/srtcore/api.cpp
  ${SRT_DIR}/srtcore/buffer_snd.cpp
  ${SRT_DIR}/srtcore/buffer_rcv.cpp
  ${SRT_DIR}/srtcore/buffer_tools.cpp
  ${SRT_DIR}/srtcore/cache.cpp
  ${SRT_DIR}/srtcore/channel.cpp
  ${SRT_DIR}/srtcore/common.cpp
  ${SRT_DIR}/srtcore/core.cpp
  ${SRT_DIR}/srtcore/crypto.cpp
  ${SRT_DIR}/srtcore/epoll.cpp
  ${SRT_DIR}/srtcore/fec.cpp
  ${SRT_DIR}/srtcore/handshake.cpp
  ${SRT_DIR}/srtcore/list.cpp
  ${SRT_DIR}/srtcore/logger_default.cpp
  ${SRT_DIR}/srtcore/logger_defs.cpp
  ${SRT_DIR}/srtcore/logging.cpp
  ${SRT_DIR}/srtcore/md5.cpp
  ${SRT_DIR}/srtcore/packet.cpp
  ${SRT_DIR}/srtcore/packetfilter.cpp
  ${SRT_DIR}/srtcore/queue.cpp
  ${SRT_DIR}/srtcore/congctl.cpp
  ${SRT_DIR}/srtcore/socketconfig.cpp
  ${SRT_DIR}/srtcore/srt_c_api.cpp
  ${SRT_DIR}/srtcore/srt_compat.c
  ${SRT_DIR}/srtcore/strerror_defs.cpp
  ${SRT_DIR}/srtcore/sync.cpp
  ${SRT_DIR}/srtcore/sync_cxx11.cpp
  ${SRT_DIR}/srtcore/tsbpd_time.cpp
  ${SRT_DIR}/srtcore/window.cpp
  ${SRT_DIR}/haicrypt/cryspr.c
  ${SRT_DIR}/haicrypt/cryspr-mbedtls.c
  ${SRT_DIR}/haicrypt/hcrypt.c
  ${SRT_DIR}/haicrypt/hcrypt_ctx_rx.c
  ${SRT_DIR}/haicrypt/hcrypt_ctx_tx.c
  ${SRT_DIR}/haicrypt/hcrypt_rx.c
  ${SRT_DIR}/haicrypt/hcrypt_sa.c
  ${SRT_DIR}/haicrypt/hcrypt_tx.c
  ${SRT_DIR}/haicrypt/hcrypt_xpt_srt.c
  ${SRT_DIR}/haicrypt/haicrypt_log.cpp
)

target_include_directories(
  srt-bundled
  PRIVATE "${_srt_inc}/srt" "${SRT_DIR}/common" "${SRT_DIR}/srtcore" "${SRT_DIR}/haicrypt"
  PUBLIC "${_srt_inc}"
)

target_compile_definitions(
  srt-bundled
  PRIVATE
    _GNU_SOURCE
    HAI_PATCH=1
    HAI_ENABLE_SRT=1
    SRT_VERSION="${SRT_VERSION}"
    SRT_ENABLE_APP_READER
    SRT_ENABLE_CLOSE_SYNCH
    SRT_ENABLE_ENCRYPTION
    USE_MBEDTLS=1
    ENABLE_STDCXX_SYNC=1
    ENABLE_MONOTONIC_CLOCK=1
    ENABLE_SOCK_CLOEXEC=1
    ENABLE_LOGGING=1
    HAVE_INET_PTON=1
    HAVE_CXX_STD_PUT_TIME=1
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

# Third-party code: don't apply OBS's warning policy to it
target_compile_options(srt-bundled PRIVATE -w)
target_compile_features(srt-bundled PUBLIC cxx_std_11)
set_target_properties(srt-bundled PROPERTIES POSITION_INDEPENDENT_CODE TRUE FOLDER deps)

target_link_libraries(
  srt-bundled
  PRIVATE MbedTLS::mbedtls MbedTLS::mbedcrypto
  PUBLIC Threads::Threads $<$<PLATFORM_ID:Haiku>:network>
)
