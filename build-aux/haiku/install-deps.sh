#!/bin/bash
# Installs the HaikuPorts packages needed to build OBS Studio on Haiku (x86_64).
# Usage: build-aux/haiku/install-deps.sh [--optional]

set -u

REQUIRED=(
	cmake ninja pkgconfig
	ffmpeg8_devel x264_devel jansson_devel curl_devel mbedtls3_devel zlib_devel uthash
	qt6_base_devel qt6_svg_devel
	mesa_devel mesa_eglvnd libglvnd_devel
	freetype_devel fontconfig_devel
	nlohmann_json libdatachannel_devel
)

# Scripting (Lua/Python) and the noise suppression filters
RECOMMENDED=(
	luajit_devel swig python3.12
	speexdsp_devel rnnoise_devel
)

# VLC video source, FDK AAC encoder, obs-websocket (still needs qrcodegencpp, see docs/haiku/BUILDING.md)
OPTIONAL=(
	vlc_devel fdk_aac_devel websocketpp asio
)

echo "==> Installing required packages"
pkgman install -y "${REQUIRED[@]}" || {
	echo "!! Installing the required packages failed. Run 'pkgman refresh' and try again;" >&2
	echo "!! if a single package is missing, install the others by hand and report the name." >&2
	exit 1
}

echo "==> Installing recommended packages (one at a time, failures are not fatal)"
for p in "${RECOMMENDED[@]}"; do
	pkgman install -y "$p" || echo "!! could not install $p (the matching feature will be disabled)"
done

if [ "${1:-}" = "--optional" ]; then
	echo "==> Installing optional packages"
	for p in "${OPTIONAL[@]}"; do
		pkgman install -y "$p" || echo "!! could not install $p"
	done
fi

echo "==> Done"
