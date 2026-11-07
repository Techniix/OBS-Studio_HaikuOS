# Builds the bundled libRIST (deps/librist, BSD-2-Clause) as a static library for Haiku.
#
# libRIST uses Meson upstream. This reproduces its library target (meson.build) for a POSIX system with
# pthreads, clock_gettime, the system mbedTLS (for SRP/EAP authentication and AES) and the builtin cJSON.
#
# Source: https://code.videolan.org/rist/librist (canonical). The submodule points at a GitHub mirror
# (github.com/nanake/librist) because code.videolan.org isn't reachable from every build environment.

include("${CMAKE_SOURCE_DIR}/cmake/haiku/haiku-patch.cmake")

set(RIST_SRC_DIR "${CMAKE_SOURCE_DIR}/deps/librist")
set(RIST_DIR "${CMAKE_CURRENT_BINARY_DIR}/librist-src")

# Haiku patches (applied to a copy in the build tree):
# - endian-shim.h: use <endian.h> like Linux (Haiku's headers/bsd provides htobe64()/be64toh()).
# - udpsocket.c: Haiku has no struct ip_mreqn; its IP_MULTICAST_IF takes the interface's IPv4 address
#   (BSD semantics), looked up with getifaddrs().
foreach(_d src include contrib)
  haiku_copy_tree("${RIST_SRC_DIR}/${_d}" "${RIST_DIR}/${_d}" EXCLUDE endian-shim.h udpsocket.c)
endforeach()
haiku_patch_file(
  "${RIST_SRC_DIR}/contrib/endian-shim.h"
  "${RIST_DIR}/contrib/endian-shim.h"
  "#elif defined(__linux__) || defined(__GNU__)"
  "#elif defined(__linux__) || defined(__GNU__) || defined(__HAIKU__)"
)
haiku_patch_file(
  "${RIST_SRC_DIR}/src/udpsocket.c"
  "${RIST_DIR}/src/udpsocket.c"
  [=[		struct ip_mreqn req = { .imr_ifindex = scope };
		return setsockopt(sd, SOL_IP, IP_MULTICAST_IF, &req, sizeof(req));]=]
  [=[#ifdef __HAIKU__
		struct ifaddrs *ifas = NULL, *ifa;
		struct in_addr addr = { .s_addr = INADDR_ANY };
		int found = 0;
		if (getifaddrs(&ifas) != 0)
			return -1;
		for (ifa = ifas; ifa; ifa = ifa->ifa_next) {
			if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET && strcmp(ifa->ifa_name, mciface) == 0) {
				addr = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
				found = 1;
				break;
			}
		}
		freeifaddrs(ifas);
		if (!found)
			return -1;
		return setsockopt(sd, IPPROTO_IP, IP_MULTICAST_IF, &addr, sizeof(addr));
#else
		struct ip_mreqn req = { .imr_ifindex = scope };
		return setsockopt(sd, SOL_IP, IP_MULTICAST_IF, &req, sizeof(req));
#endif]=]
  [=[#include "log-private.h"]=]
  [=[#include "log-private.h"
#ifdef __HAIKU__
#include <ifaddrs.h>
#include <string.h>
#endif]=]
)
set(RIST_VERSION_STRING "v0.2.12-haiku")

find_package(MbedTLS 3...<4 REQUIRED)
find_package(Threads REQUIRED)

set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")

# config.h (meson configuration_data)
file(
  WRITE "${_gen}/config.h.tmp"
  "#pragma once\n"
  "#define ALLOW_INSECURE_IV_FALLBACK 0\n"
  "#define HAVE_CLOCK_GETTIME 1\n"
  "#define HAVE_PTHREADS 1\n"
  "#define HAVE_SOCK_UN_H 1\n"
  "#define HAVE_LIBMICROHTTPD 0\n"
  "#define HAVE_PROMETHEUS_SUPPORT 1\n"
  "#define HAVE_MBEDTLS 1\n"
  "#define HAVE_NETTLE 0\n"
  "#define MBEDTLS_HAS_MPI_RANDOM 1\n"
)
configure_file("${_gen}/config.h.tmp" "${_gen}/config.h" COPYONLY)

# vcs_version.h (meson vcs_tag)
set(VCS_TAG "${RIST_VERSION_STRING}")
configure_file("${RIST_SRC_DIR}/include/vcs_version.h.in" "${_gen}/vcs_version.h" @ONLY)

# Public headers as <librist/...>, like an installed libRIST
set(LIBRIST_API_VERSION_MAJOR 4)
set(LIBRIST_API_VERSION_MINOR 6)
set(LIBRIST_API_VERSION_PATCH 0)
set(LIBRIST_API_VERSION 0x40600)
set(_inc "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file("${RIST_DIR}/include/librist/version.h.in" "${_inc}/librist/version.h" @ONLY)
set(HAVE_SRP_SUPPORT 1)
configure_file("${RIST_DIR}/include/librist/config.h.in" "${_inc}/librist/librist_config.h" @ONLY)
foreach(
  _h
  common.h
  headers.h
  librist_srp.h
  librist.h
  logging.h
  oob.h
  opt.h
  peer.h
  receiver.h
  sender.h
  stats.h
  udpsocket.h
  urlparam.h
)
  configure_file("${RIST_DIR}/include/librist/${_h}" "${_inc}/librist/${_h}" COPYONLY)
endforeach()

add_library(
  librist-bundled
  STATIC
  ${RIST_DIR}/src/crypto/crypto.c
  ${RIST_DIR}/src/crypto/psk.c
  ${RIST_DIR}/src/proto/gre.c
  ${RIST_DIR}/src/proto/rtp.c
  ${RIST_DIR}/src/proto/rist_time.c
  ${RIST_DIR}/src/flow.c
  ${RIST_DIR}/src/logging.c
  ${RIST_DIR}/src/network.c
  ${RIST_DIR}/src/rist.c
  ${RIST_DIR}/src/rist-common.c
  ${RIST_DIR}/src/rist_ref.c
  ${RIST_DIR}/src/rist-thread.c
  ${RIST_DIR}/src/mpegts.c
  ${RIST_DIR}/src/peer.c
  ${RIST_DIR}/src/udp.c
  ${RIST_DIR}/src/stats.c
  ${RIST_DIR}/src/udpsocket.c
  ${RIST_DIR}/src/libevsocket.c
  ${RIST_DIR}/contrib/stdio-shim.c
  ${RIST_DIR}/contrib/time-shim.c
  ${RIST_DIR}/contrib/pthread-shim.c
  # SRP/EAP authentication (with mbedTLS)
  ${RIST_DIR}/src/proto/eap.c
  ${RIST_DIR}/src/crypto/srp_constants.c
  ${RIST_DIR}/src/crypto/srp.c
  ${RIST_DIR}/src/crypto/random.c
  # builtin cJSON
  ${RIST_DIR}/contrib/contrib_cJSON/cjson/cJSON.c
)

target_include_directories(
  librist-bundled
  # Same order as meson.build ('.', 'src', 'include/librist', 'include', 'contrib'): src/peer.h must win over
  # the public include/librist/peer.h
  PRIVATE
    "${_gen}"
    "${RIST_DIR}"
    "${RIST_DIR}/src"
    "${_inc}/librist"
    "${RIST_DIR}/include/librist"
    "${RIST_DIR}/include"
    "${RIST_DIR}/contrib"
    "${RIST_DIR}/contrib/contrib_cJSON"
  PUBLIC "${_inc}"
)

target_compile_definitions(librist-bundled PRIVATE _DEFAULT_SOURCE CJSON_HIDE_SYMBOLS)
target_compile_options(librist-bundled PRIVATE -w)
set_target_properties(librist-bundled PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE TRUE FOLDER deps)

target_link_libraries(
  librist-bundled
  PRIVATE MbedTLS::mbedcrypto
  PUBLIC Threads::Threads $<$<PLATFORM_ID:Haiku>:network>
)
