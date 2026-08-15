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

echo "--- rendering Netflix Playback OSD, Dialog & Settings screens"
export LD_LIBRARY_PATH="/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build:${LD_LIBRARY_PATH:-}"
./output/uiview/uiview_playback_rml

python3 tools/shot.py png output/uiview/rml_playback.bmp output/uiview/rml_playback.png
python3 tools/shot.py png output/uiview/rml_dialog.bmp output/uiview/rml_dialog.png
python3 tools/shot.py png output/uiview/rml_settings.bmp output/uiview/rml_settings.png
python3 tools/shot.py png output/uiview/rml_settings_sub.bmp output/uiview/rml_settings_sub.png
echo "  ok -> output/uiview/rml_playback.png"
echo "  ok -> output/uiview/rml_dialog.png"
echo "  ok -> output/uiview/rml_settings.png"
echo "  ok -> output/uiview/rml_settings_sub.png"
