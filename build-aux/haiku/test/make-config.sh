#!/bin/bash
# Writes a throwaway OBS configuration that lets OBS start unattended and record a known scene.
# Used by build-aux/haiku/test.sh; can be run by hand to inspect what the test feeds OBS:
#
#     build-aux/haiku/test/make-config.sh <xdg-config-home> <recording-dir> [with-text: 1|0]
#
# libobs resolves every configuration path through os_get_config_path(), which prefers $XDG_CONFIG_HOME over
# $HOME/config/settings (libobs/util/platform-nix.c:241-262), so pointing XDG_CONFIG_HOME at <xdg-config-home>
# keeps the user's real ~/config/settings/obs-studio untouched.
#
# Every key written below is one the frontend reads; the source line that reads it is cited next to it. Keys
# that are not written keep their built-in defaults.

set -u

# ---------------------------------------------------------------------------------------------------------
# IDS: everything a variant of this branch (e.g. the Renga one) might need to change lives in this block.
# ---------------------------------------------------------------------------------------------------------
PROFILE_NAME="HaikuTest"                         # --profile argument, basic/profiles/<dir>
COLLECTION_NAME="HaikuTest"                      # --collection argument, basic/scenes/<file>.json
SCENE_NAME="Test Scene"

ID_SCREEN_CAPTURE="haiku_screen_capture"         # plugins/haiku-capture/screen-capture.cpp:204
KEY_SCREEN_CURSOR="capture_cursor"               # plugins/haiku-capture/screen-capture.cpp:40 (S_CAPTURE_CURSOR)
KEY_SCREEN_FPS="fps"                             # plugins/haiku-capture/screen-capture.cpp:41 (S_FPS)
KEY_SCREEN_METHOD="capture_method"               # plugins/haiku-capture/capture-throttle.hpp:178; 0 = tear free (:174)

ID_DESKTOP_AUDIO="haiku_desktop_audio_capture"   # frontend/OBSApp.cpp:1437 (OUTPUT_AUDIO_SOURCE),
                                                 # plugins/haiku-media/desktop-audio.cpp:544
KEY_AUDIO_DEVICE="device_id"                     # plugins/haiku-media/desktop-audio.cpp:477, default "default" (:481)
JSON_DESKTOP_AUDIO_KEY="DesktopAudioDevice1"     # frontend/widgets/OBSBasic_SceneCollections.cpp:798, loaded onto
                                                 # output channel 1 at :1259

ID_COLOR="color_source"                          # plugins/image-source/color-source.c:166
ID_COLOR_VERSIONED="color_source_v3"             # same, .version = 3 (:167)
ID_TEXT="text_ft2_source"                        # plugins/text-freetype2/text-freetype2.c:71
ID_TEXT_VERSIONED="text_ft2_source_v2"           # same, .version = 2 (:72)

CANVAS_CX=1280
CANVAS_CY=720
FPS=30
VIDEO_BITRATE=1500                               # kbit/s; low so a 20 s recording stays well under 8 MB
# ---------------------------------------------------------------------------------------------------------

if [ $# -lt 2 ]; then
	echo "usage: $0 <xdg-config-home> <recording-dir> [with-text: 1|0]" >&2
	exit 2
fi
XDG="$1"
REC_DIR="$2"
WITH_TEXT="${3:-1}"

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../../.." && pwd)"

# LIBOBS_API_VER = (major << 24) | (minor << 16) | patch (libobs/obs-config.h:30-48). Falls back to 33.0.1.
api_part() {
	local v
	v="$(sed -n "s/^#define LIBOBS_API_$1_VER[[:space:]]*\([0-9][0-9]*\).*/\1/p" "$SRC/libobs/obs-config.h" 2>/dev/null |
		head -n 1)"
	[ -n "$v" ] && echo "$v" || echo "$2"
}
API_VER=$(( ($(api_part MAJOR 33) << 24) | ($(api_part MINOR 0) << 16) | $(api_part PATCH 1) ))
INFO_VER=$(( (API_VER << 16) | 65535 ))

# Minimal JSON string escaping for the paths and names substituted below.
json_escape() {
	printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

BASE="$XDG/obs-studio"
mkdir -p "$BASE/basic/profiles/$PROFILE_NAME" "$BASE/basic/scenes" "$REC_DIR" || exit 1

# ---------------------------------------------------------------------------------------------------------
# global.ini (application-wide settings; frontend/OBSApp.cpp:576 opens obs-studio/global.ini)
# ---------------------------------------------------------------------------------------------------------
# LastVersion          frontend/OBSApp.cpp:587 reads it for the pre-31 migration (skipped: not below 31.0);
#                      frontend/widgets/OBSBasic.cpp:1160 "has_last_version": with a value present the
#                      auto-configuration wizard is not queued (:1168).
# Pre33.0Migrated      frontend/OBSApp.cpp:705, marks the 33.0 snap-distance migration as done.
# EnableAutoUpdates    frontend/OBSApp.cpp:337, read at frontend/widgets/OBSBasic_Updater.cpp:179. --disable-updater
#                      (frontend/obs-main.cpp:1018) already turns the updater off; this is the second lock.
# InfoIncrement,       frontend/widgets/OBSBasic_Updater.cpp:126-137: the "what's new" page is shown only if the
# InfoLast*Version     server's increment is larger than InfoIncrement, and InfoIncrement is reset when the stored
#                      Info*Version is older than this build. Both are seeded past any real value. (The dialog
#                      only exists in builds with the browser panel: frontend/widgets/OBSBasic.cpp:1257.)
cat > "$BASE/global.ini" <<EOF
[General]
LastVersion=$API_VER
Pre33.0Migrated=true
EnableAutoUpdates=false
InfoIncrement=2147483647
InfoLastVersion=$INFO_VER
InfoLastRCVersion=$INFO_VER
InfoLastBetaVersion=$INFO_VER
EOF

# ---------------------------------------------------------------------------------------------------------
# user.ini (per-user settings; frontend/OBSApp.cpp:648 opens <Locations/Configuration>/obs-studio/user.ini, and
# that location defaults to the config path itself: frontend/OBSApp.cpp:370-376)
# ---------------------------------------------------------------------------------------------------------
# General/FirstRun           frontend/widgets/OBSBasic.cpp:1161: true = not a first run, so no auto-configuration
#                            wizard (:1168); also read at frontend/widgets/OBSBasic_SceneCollections.cpp:1159.
# General/ConfirmOnExit      frontend/OBSApp.cpp:386 (default true), read at frontend/widgets/OBSBasic.cpp:1804:
#                            with it on, quitting while recording opens a confirmation box, and the test quits
#                            OBS by SIGTERM while it records.
# Basic/Profile, ProfileDir  frontend/OBSApp.cpp:1153-1154; frontend/widgets/OBSBasic.cpp:794 selects the profile
#                            (--profile, frontend/obs-main.cpp:1000, takes precedence but names the same one).
# Basic/SceneCollection,     frontend/OBSApp.cpp:1155-1156; frontend/widgets/OBSBasic.cpp:996 (--collection,
# SceneCollectionFile        frontend/obs-main.cpp:995, likewise).
# Basic/ConfigOnNewProfile   frontend/OBSApp.cpp:1157, read at frontend/widgets/OBSBasic_Profiles.cpp:423: no wizard
#                            if OBS ever has to create a profile.
# BasicWindow/SysTrayEnabled,     frontend/OBSApp.cpp:402-403, read at frontend/widgets/OBSBasic.cpp:1046-1049 and
# SysTrayWhenStarted              frontend/widgets/OBSBasic_SysTray.cpp:118-126: no tray icon, window always shown.
# BasicWindow/PreviewEnabled      frontend/widgets/OBSBasic.cpp:1023: keep the preview on, it is the on-screen
#                                 display path the GL test wants exercised.
# BasicWindow/SaveProjectors      frontend/widgets/OBSBasic_SceneCollections.cpp:1399: never reopen projectors.
cat > "$BASE/user.ini" <<EOF
[General]
FirstRun=true
ConfirmOnExit=false

[Basic]
Profile=$PROFILE_NAME
ProfileDir=$PROFILE_NAME
SceneCollection=$COLLECTION_NAME
SceneCollectionFile=$COLLECTION_NAME
ConfigOnNewProfile=false

[BasicWindow]
SysTrayEnabled=false
SysTrayWhenStarted=false
PreviewEnabled=true
SaveProjectors=false
EOF

# ---------------------------------------------------------------------------------------------------------
# basic/profiles/<dir>/basic.ini (frontend/widgets/OBSBasic_Profiles.cpp:35-36; a directory is a profile if
# it holds a basic.ini, and General/Name is the profile name: OBSBasic_Profiles.cpp:351-367)
# ---------------------------------------------------------------------------------------------------------
# Output/Mode                 frontend/widgets/OBSBasic.cpp:629 ("Simple")
# SimpleOutput/FilePath       frontend/widgets/OBSBasic.cpp:636, used at frontend/utility/SimpleOutput.cpp:829
# SimpleOutput/RecFormat2     frontend/widgets/OBSBasic.cpp:637, used at frontend/utility/SimpleOutput.cpp:830
# SimpleOutput/RecQuality     frontend/widgets/OBSBasic.cpp:643; "Stream" = record with the stream encoder
#                             (frontend/utility/SimpleOutput.cpp:138)
# SimpleOutput/StreamEncoder, frontend/widgets/OBSBasic.cpp:772-775; "x264" (SIMPLE_ENCODER_X264,
# RecEncoder                  frontend/widgets/OBSBasic.hpp:77) maps to obs_x264 (frontend/utility/SimpleOutput.cpp:93).
#                             Set explicitly so an available NVENC is never picked.
# SimpleOutput/VBitrate, ABitrate, Preset   frontend/widgets/OBSBasic.cpp:638-641
# Video/BaseCX..OutputCY      frontend/widgets/OBSBasic.cpp:691-737, read at :1461-1464
# Video/FPSType, FPSCommon    frontend/widgets/OBSBasic.cpp:741-742; type 0 = "common" value (:1721-1733)
# Audio/SampleRate, ChannelSetup   frontend/widgets/OBSBasic.cpp:757-758
cat > "$BASE/basic/profiles/$PROFILE_NAME/basic.ini" <<EOF
[General]
Name=$PROFILE_NAME

[Output]
Mode=Simple

[SimpleOutput]
FilePath=$REC_DIR
RecFormat2=mkv
RecQuality=Stream
StreamEncoder=x264
RecEncoder=x264
VBitrate=$VIDEO_BITRATE
ABitrate=160
Preset=veryfast

[Video]
BaseCX=$CANVAS_CX
BaseCY=$CANVAS_CY
OutputCX=$CANVAS_CX
OutputCY=$CANVAS_CY
FPSType=0
FPSCommon=$FPS

[Audio]
SampleRate=48000
ChannelSetup=Stereo
EOF

# ---------------------------------------------------------------------------------------------------------
# basic/scenes/<file>.json (frontend/widgets/OBSBasic_SceneCollections.cpp:45; every *.json in that directory
# is a collection and its "name" member is the collection name: :439-460)
# ---------------------------------------------------------------------------------------------------------
# Top level, as written by OBSBasic::Save (OBSBasic_SceneCollections.cpp:806-990) and read by
# OBSBasic::LoadData (:1190-1500):
#   name                              :812 / :1249
#   DesktopAudioDevice1               :826 / :1259  an obs_save_source() object, put on output channel 1
#   sources, groups                   :886-887 / :1216-1217, loaded with obs_load_sources (:1346)
#   scene_order                       :891 / :1215, items { "name": ... } (frontend/widgets/OBSBasic_Scenes.cpp:94)
#   current_scene,                    :900-901 / :1220-1221
#   current_program_scene
#   transition_duration               :913 / :1231 (current_transition is left out: FindTransition fails and the
#                                     default fade is used, :1360-1363)
#   transitions, quick_transitions,   :914-915, :919 / :1218, :1416 (empty arrays)
#   saved_projectors
#   preview_locked, scaling_enabled   :922-923 / :1420, :1426
#   modules                           :951 / :1205
#   version                           :986 / :1279; 2 = relative coordinates (:1319-1327)
#
# A source object (obs_load_source_type, libobs/obs.c:2257-2331): id, versioned_id, name, uuid, settings, plus
# volume/mixers/muted/enabled, which default to 1.0 / 0x3F / false / true when absent.
#
# A scene is a source with id "scene" whose settings hold "items" (libobs/obs-scene.c:1276). A scene item
# (scene_load_item, libobs/obs-scene.c:1118-1262): name, source_uuid (falls back to name, :1135-1140), id,
# visible (no default, so it must be true), locked, pos, scale, rot, align (default top-left = 5),
# bounds_type/bounds_align/bounds. The items carry absolute "pos"/"bounds" and no "*_rel" members; in a
# version 2 collection libobs converts them itself (libobs/obs-scene.c:1182-1185 and :1204-1206).
# bounds_type 2 = OBS_BOUNDS_SCALE_INNER (libobs/obs.h:156).

if [ "$WITH_TEXT" = "1" ]; then
	TEXT_SOURCE=$(cat <<EOF
    {
      "id": "$ID_TEXT",
      "versioned_id": "$ID_TEXT_VERSIONED",
      "name": "Test Text",
      "uuid": "0b500000-0000-4000-8000-000000000003",
      "settings": { "text": "OBS Haiku automated test" }
    },
EOF
)
	TEXT_ITEM=$(cat <<EOF
,
          {
            "name": "Test Text", "source_uuid": "0b500000-0000-4000-8000-000000000003", "id": 3,
            "visible": true, "locked": false, "rot": 0.0, "align": 5,
            "pos": { "x": 40.0, "y": 200.0 }, "scale": { "x": 1.0, "y": 1.0 },
            "bounds_type": 0, "bounds_align": 0, "bounds": { "x": 0.0, "y": 0.0 }
          }
EOF
)
else
	TEXT_SOURCE=""
	TEXT_ITEM=""
fi

cat > "$BASE/basic/scenes/$COLLECTION_NAME.json" <<EOF
{
  "name": "$(json_escape "$COLLECTION_NAME")",
  "$JSON_DESKTOP_AUDIO_KEY": {
    "id": "$ID_DESKTOP_AUDIO",
    "versioned_id": "$ID_DESKTOP_AUDIO",
    "name": "Desktop Audio",
    "uuid": "0b500000-0000-4000-8000-000000000010",
    "settings": { "$KEY_AUDIO_DEVICE": "default" },
    "mixers": 255,
    "volume": 1.0,
    "muted": false,
    "enabled": true
  },
  "sources": [
    {
      "id": "$ID_SCREEN_CAPTURE",
      "versioned_id": "$ID_SCREEN_CAPTURE",
      "name": "Test Screen Capture",
      "uuid": "0b500000-0000-4000-8000-000000000001",
      "settings": { "$KEY_SCREEN_CURSOR": true, "$KEY_SCREEN_FPS": $FPS, "$KEY_SCREEN_METHOD": 0 }
    },
    {
      "id": "$ID_COLOR",
      "versioned_id": "$ID_COLOR_VERSIONED",
      "name": "Test Colour",
      "uuid": "0b500000-0000-4000-8000-000000000002",
      "settings": { "color": 4280303840, "width": 200, "height": 120 }
    },
$TEXT_SOURCE
    {
      "id": "scene",
      "versioned_id": "scene",
      "name": "$(json_escape "$SCENE_NAME")",
      "uuid": "0b500000-0000-4000-8000-000000000020",
      "settings": {
        "custom_size": false,
        "id_counter": 3,
        "items": [
          {
            "name": "Test Screen Capture", "source_uuid": "0b500000-0000-4000-8000-000000000001", "id": 1,
            "visible": true, "locked": false, "rot": 0.0, "align": 5,
            "pos": { "x": 0.0, "y": 0.0 }, "scale": { "x": 1.0, "y": 1.0 },
            "bounds_type": 2, "bounds_align": 0, "bounds": { "x": $CANVAS_CX.0, "y": $CANVAS_CY.0 }
          },
          {
            "name": "Test Colour", "source_uuid": "0b500000-0000-4000-8000-000000000002", "id": 2,
            "visible": true, "locked": false, "rot": 0.0, "align": 5,
            "pos": { "x": 40.0, "y": 40.0 }, "scale": { "x": 1.0, "y": 1.0 },
            "bounds_type": 0, "bounds_align": 0, "bounds": { "x": 0.0, "y": 0.0 }
          }$TEXT_ITEM
        ]
      }
    }
  ],
  "groups": [],
  "scene_order": [ { "name": "$(json_escape "$SCENE_NAME")" } ],
  "current_scene": "$(json_escape "$SCENE_NAME")",
  "current_program_scene": "$(json_escape "$SCENE_NAME")",
  "transition_duration": 300,
  "transitions": [],
  "quick_transitions": [],
  "saved_projectors": [],
  "preview_locked": false,
  "scaling_enabled": false,
  "modules": {},
  "version": 2
}
EOF

# Values the caller needs, in a form it can eval.
echo "CFG_PROFILE='$PROFILE_NAME'"
echo "CFG_COLLECTION='$COLLECTION_NAME'"
echo "CFG_FPS=$FPS"
echo "CFG_CX=$CANVAS_CX"
echo "CFG_CY=$CANVAS_CY"
