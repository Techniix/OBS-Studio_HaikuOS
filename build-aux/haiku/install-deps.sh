#!/bin/bash
# Installs the HaikuPorts packages needed to build OBS Studio on Haiku (x86_64).
# Usage: build-aux/haiku/install-deps.sh [--optional]

set -u

REQUIRED=(
	cmake ninja pkgconfig
	ffmpeg8_devel x264_devel jansson_devel curl_devel mbedtls3_devel zlib_devel uthash
	qt6_base_devel qt6_svg_devel
	haiku_devel
	mesa_devel
	freetype_devel fontconfig_devel
	nlohmann_json libdatachannel_devel
)

# Scripting (Lua/Python), the noise suppression filters, the VLC video source and the browser source/docks
# (build.sh turns off whatever is missing). cmd:python3 is whichever Python HaikuPorts currently installs as
# the default python3 (3.14 at the time of writing); its headers come with the main package.
RECOMMENDED=(
	luajit_devel swig cmd:python3
	speexdsp_devel rnnoise_devel
	vlc_devel
	haikuwebkit_devel
)

# FDK AAC encoder (Asio, WebSocket++, qrcodegen, SRT and RIST are bundled in deps/)
OPTIONAL=(
	fdk_aac_devel
)

echo "==> Installing required packages"
if ! pkgman install -y "${REQUIRED[@]}"; then
	# pkgman rejects the whole list if one name is unknown; retry one by one to find out which
	echo "==> Retrying the required packages one at a time"
	failed=()
	for p in "${REQUIRED[@]}"; do
		pkgman install -y "$p" || failed+=("$p")
	done
	if [ ${#failed[@]} -gt 0 ]; then
		echo "!! These required packages could not be installed: ${failed[*]}" >&2
		echo "!! Run 'pkgman refresh' and try again; if a name is reported as not found, report it." >&2
		exit 1
	fi
fi

# The Gallium software pipe renderer is a sub-package of the Mesa 22 recipe only (Mesa 25 replaces it with
# mesa_eglvnd), so it is not required: asking for it on a system with Mesa 25 would fail the required list.
pkgman install -y mesa_swpipe >/dev/null 2>&1 && echo "==> Mesa software pipe renderer (mesa_swpipe) installed"

# Mesa 25 adds EGL for Haiku (mesa_eglvnd + libglvnd). It isn't published yet; OBS uses OSMesa from Mesa 22
# until it is, and picks EGL up automatically once these packages exist.
pkgman install -y mesa_eglvnd libglvnd_devel >/dev/null 2>&1 && echo "==> Mesa EGL (mesa_eglvnd) installed"

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
