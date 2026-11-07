#!/bin/bash
# Prepares the Haiku cross-check environment in $HCHECK_WORK (default: ./work next to this script):
#   haiku/    Haiku system headers (sparse clone of haiku/haiku)
#   deps/     dependency sources (headers only where possible)
#   sysroot/  fake Haiku sysroot: /boot/system/develop/{headers,lib} + /boot/system/lib/cmake
#   qt6/      Qt 6 built for the host (headers, CMake packages, moc/uic/rcc)
#
# Needs: git, cmake >= 3.28, ninja, clang, a host C++17 compiler for Qt, ~4 GB disk.
# Building Qt takes 1-2 hours on 2 cores; pass --no-qt to skip it (then configure with -DENABLE_FRONTEND=OFF
# and without frontend-tools).

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${HCHECK_WORK:-$HERE/work}"
QT_VERSION=v6.10.1
BUILD_QT=1
[ "${1:-}" = "--no-qt" ] && BUILD_QT=0

mkdir -p "$WORK/deps"
cd "$WORK"

clone() { # name url ref [sparse paths...]
	local name=$1 url=$2 ref=$3
	shift 3
	[ -d "$name/.git" ] && return 0
	if [ $# -gt 0 ]; then
		git clone -q --depth 1 --filter=blob:none --sparse -b "$ref" "$url" "$name"
		git -C "$name" sparse-checkout set "$@"
	else
		git clone -q --depth 1 -b "$ref" "$url" "$name"
	fi
}

echo "==> Haiku headers"
clone haiku https://github.com/haiku/haiku master headers

echo "==> Dependency sources"
cd deps
clone ffmpeg https://github.com/FFmpeg/FFmpeg n8.0 libavcodec libavformat libavutil libswscale libswresample libavdevice libavfilter
clone jansson https://github.com/akheron/jansson v2.14
clone zlib https://github.com/madler/zlib v1.3.1
clone curl https://github.com/curl/curl curl-8_11_0 include
clone mbedtls https://github.com/Mbed-TLS/mbedtls v3.6.2 include
clone x264 https://github.com/mirror/x264 stable
clone freetype https://github.com/freetype/freetype VER-2-13-3 include
clone egl https://github.com/KhronosGroup/EGL-Registry main api
clone json https://github.com/nlohmann/json v3.11.3 single_include
clone vlc https://github.com/videolan/vlc 3.0.21 include/vlc
clone libdatachannel https://github.com/paullouisageneau/libdatachannel v0.22.5 include
clone speexdsp https://github.com/xiph/speexdsp SpeexDSP-1.2.1 include
clone rnnoise https://github.com/xiph/rnnoise main include
clone luajit https://github.com/LuaJIT/LuaJIT v2.1 src
clone uthash https://github.com/troydhanson/uthash master
clone haikuwebkit https://github.com/haiku/haikuwebkit haiku Source/WebKitLegacy/haiku/API
cd ..

echo "==> Sysroot"
S="$WORK/sysroot"
I="$S/boot/system/develop/headers"
L="$S/boot/system/develop/lib"
C="$S/boot/system/lib/cmake"
rm -rf "$S"
mkdir -p "$I" "$L" "$C"

for l in libavcodec libavformat libavutil libswscale libswresample libavdevice libavfilter; do
	(cd "deps/ffmpeg/$l" && find . -name '*.h' -not -path '*/x86/*' -not -path '*/arm/*' \
		-not -path '*/aarch64/*' -not -path '*/tests/*' | while read -r f; do
		mkdir -p "$I/$l/$(dirname "$f")"
		cp "$f" "$I/$l/$f"
	done)
done
printf '#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif\n' > "$I/libavutil/avconfig.h"
echo '#define FFMPEG_VERSION "n8.0"' > "$I/libavutil/ffversion.h"

sed -e 's/@json_inline@/inline/; s/@json_have_long_long@/1/; s/@json_have_localeconv@/1/' \
	-e 's/@json_have_atomic_builtins@/1/; s/@json_have_sync_builtins@/1/; s/@json_have_strtoll@/1/' \
	deps/jansson/src/jansson_config.h.in > "$I/jansson_config.h"
cp deps/jansson/src/jansson.h "$I/"
cp deps/zlib/zlib.h deps/zlib/zconf.h "$I/"
cp -r deps/curl/include/curl "$I/"
cp -r deps/mbedtls/include/mbedtls deps/mbedtls/include/psa "$I/"
cp deps/x264/x264.h "$I/"
printf '#define X264_GPL 1\n#define X264_INTERLACED 1\n#define X264_BIT_DEPTH 0\n#define X264_CHROMA_FORMAT 0\n#define X264_VERSION ""\n#define X264_POINTVER "0.164.x"\n' > "$I/x264_config.h"
mkdir -p "$I/freetype2" && cp -r deps/freetype/include/* "$I/freetype2/"
mkdir -p "$I/fontconfig" && cp "$HERE/fake-configs/fontconfig.h" "$I/fontconfig/"
cp -r deps/egl/api/EGL deps/egl/api/KHR "$I/"
mkdir -p "$I/GL" && printf '#ifndef __gl_h_\n#define __gl_h_\n#endif\n' > "$I/GL/gl.h"
cp -r deps/json/single_include/nlohmann "$I/"
cp -r deps/libdatachannel/include/rtc "$I/"
cp -r deps/speexdsp/include/speex "$I/"
printf '#ifndef __SPEEX_TYPES_H__\n#define __SPEEX_TYPES_H__\n#include <stdint.h>\ntypedef int16_t spx_int16_t;\ntypedef uint16_t spx_uint16_t;\ntypedef int32_t spx_int32_t;\ntypedef uint32_t spx_uint32_t;\n#endif\n' > "$I/speex/speexdsp_config_types.h"
cp deps/rnnoise/include/rnnoise.h "$I/"
cp -r deps/vlc/include/vlc "$I/"
cp deps/uthash/src/*.h "$I/"
mkdir -p "$I/luajit-2.1"
cp deps/luajit/src/{lua.h,lualib.h,lauxlib.h,luaconf.h,lua.hpp} "$I/luajit-2.1/"
cp deps/luajit/src/luajit_rolling.h "$I/luajit-2.1/luajit.h"

# Haiku's private network services headers (haiku_devel ships them; HaikuWebKit's API headers need UrlContext.h)
mkdir -p "$I/private/netservices"
cp haiku/headers/private/netservices/*.h "$I/private/netservices/"

# HaikuWebKit: the headers haikuwebkit_devel installs (develop/headers)
for h in WebView.h WebPage.h WebWindow.h WebSettings.h WebViewConstants.h WebKitInfo.h WebDownload.h; do
	cp "deps/haikuwebkit/Source/WebKitLegacy/haiku/API/$h" "$I/"
done

# Stub libraries: find_library() only needs the files to exist; the harness never links.
for l in avcodec avformat avutil swscale swresample avdevice avfilter jansson z curl x264 freetype fontconfig \
	EGL GL speexdsp rnnoise luajit-5.1 mbedtls mbedcrypto mbedx509 datachannel vlc WebKitLegacy JavaScriptCore; do
	: > "$L/lib$l.so"
done

for d in "$HERE"/fake-configs/*/; do
	n="$(basename "$d")"
	mkdir -p "$C/$n"
	for f in "$d"*; do sed "s|@SYSROOT@|$S|g" "$f" > "$C/$n/$(basename "$f")"; done
done

if [ $BUILD_QT = 1 ] && [ ! -f "$WORK/qt6/lib/cmake/Qt6/Qt6Config.cmake" ]; then
	echo "==> Qt $QT_VERSION (host build, this takes a while)"
	clone qtbase https://github.com/qt/qtbase $QT_VERSION
	clone qtsvg https://github.com/qt/qtsvg $QT_VERSION
	cmake -S qtbase -B qtbase-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$WORK/qt6" \
		-DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DBUILD_WITH_PCH=OFF -DINPUT_opengl=no \
		-DFEATURE_opengl=OFF -DFEATURE_vulkan=OFF -DFEATURE_xcb=OFF -DFEATURE_xlib=OFF -DFEATURE_dbus=OFF \
		-DFEATURE_sql=OFF -DFEATURE_testlib=OFF -DFEATURE_printsupport=OFF -DFEATURE_concurrent=OFF \
		-DFEATURE_openssl=OFF -DFEATURE_icu=OFF -DFEATURE_glib=OFF -DFEATURE_fontconfig=OFF \
		-DFEATURE_system_freetype=OFF -DFEATURE_system_harfbuzz=OFF -DFEATURE_system_png=OFF -DFEATURE_jpeg=OFF \
		-DFEATURE_gif=OFF -DFEATURE_libinput=OFF -DFEATURE_evdev=OFF -DFEATURE_linuxfb=OFF -DFEATURE_eglfs=OFF \
		-DFEATURE_xkbcommon=OFF -DFEATURE_system_zlib=OFF -DFEATURE_system_pcre2=OFF -DFEATURE_zstd=OFF \
		-DFEATURE_brotli=OFF -DFEATURE_gssapi=OFF -DFEATURE_libproxy=OFF
	cmake --build qtbase-build
	cmake --install qtbase-build
	cmake -S qtsvg -B qtsvg-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$WORK/qt6" \
		-DCMAKE_INSTALL_PREFIX="$WORK/qt6"
	cmake --build qtsvg-build
	cmake --install qtsvg-build
fi

echo "==> Ready. Next: $HERE/run.sh <obs-source-dir>"
