#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${REPO_ROOT}/output/uiview"

if [[ ! -f /.dockerenv ]]; then
    docker compose run --rm ps5-dev bash ./tools/uiview_playback_rml.sh
    exit $?
fi

cd "${REPO_ROOT}"

echo "--- building uiview_playback_rml"
g++ -O2 -std=c++17 \
    -Iprojects/evoplayer \
    -Iprojects/evoplayer/ui_rml/include \
    -Iprojects/evoplayer/include \
    -Ibuild/rmlui-host/RmlUi/Include \
    -Ibuild/rmlui-host-dist/include/freetype2 \
    -o output/uiview/uiview_playback_rml \
    tools/uiview_playback_rml.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_render.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_system.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_app.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bridge.cpp \
    -Lbuild/rmlui-host-dist/lib \
    -Lbuild/rmlui-host/RmlUi/build \
    -lrmlui -lfreetype -lpng16 -lz -lpthread \
    -Wl,-rpath,/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build

echo "--- rendering all launch + settings screenshots"
export LD_LIBRARY_PATH="/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build:${LD_LIBRARY_PATH:-}"
./output/uiview/uiview_playback_rml

for s in rml_launch_hero rml_launch_recent rml_launch_library rml_launch_empty rml_launch_rail rml_recent rml_favorites rml_favorites_empty rml_emby_setup rml_emby_browse rml_browser rml_browser_empty rml_playback_paused rml_changelog; do
    python3 tools/shot.py png "output/uiview/$s.bmp" "output/uiview/$s.png"
    echo "  ok -> output/uiview/$s.png"
done

python3 tools/shot.py png output/uiview/rml_settings_main.bmp output/uiview/rml_settings_main.png
python3 tools/shot.py png output/uiview/rml_settings_playback.bmp output/uiview/rml_settings_playback.png
python3 tools/shot.py png output/uiview/rml_settings_subtitles.bmp output/uiview/rml_settings_subtitles.png
python3 tools/shot.py png output/uiview/rml_settings_interface.bmp output/uiview/rml_settings_interface.png
python3 tools/shot.py png output/uiview/rml_settings_system.bmp output/uiview/rml_settings_system.png
python3 tools/shot.py png output/uiview/rml_settings_profile.bmp output/uiview/rml_settings_profile.png
python3 tools/shot.py png output/uiview/rml_settings_devtools.bmp output/uiview/rml_settings_devtools.png
python3 tools/shot.py png output/uiview/rml_settings_about.bmp output/uiview/rml_settings_about.png
python3 tools/shot.py png output/uiview/rml_settings_theme.bmp output/uiview/rml_settings_theme.png
python3 tools/shot.py png output/uiview/rml_theme_carbon.bmp output/uiview/rml_theme_carbon.png
python3 tools/shot.py png output/uiview/rml_theme_ember.bmp output/uiview/rml_theme_ember.png
python3 tools/shot.py png output/uiview/rml_theme_aurora.bmp output/uiview/rml_theme_aurora.png

echo "  ok -> output/uiview/rml_settings_main.png"
echo "  ok -> output/uiview/rml_settings_playback.png"
echo "  ok -> output/uiview/rml_settings_subtitles.png"
echo "  ok -> output/uiview/rml_settings_interface.png"
echo "  ok -> output/uiview/rml_settings_system.png"
echo "  ok -> output/uiview/rml_settings_profile.png"
echo "  ok -> output/uiview/rml_settings_devtools.png"
echo "  ok -> output/uiview/rml_settings_about.png"
echo "  ok -> output/uiview/rml_settings_theme.png"
echo "  ok -> output/uiview/rml_theme_carbon.png"
echo "  ok -> output/uiview/rml_theme_ember.png"
echo "  ok -> output/uiview/rml_theme_aurora.png"
