#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${REPO_ROOT}/output/uiview"

if [[ ! -f /.dockerenv ]]; then
    docker compose run --rm ps5-dev bash ./tools/uiview_playback_rml.sh
    exit $?
fi

cd "${REPO_ROOT}"

# #60: regenerate the embedded RmlUi asset bundle before compiling, so the
# host preview always renders whatever assets/{rml,fonts,icons} currently
# contains - see tools/bundle_rml_assets.py.
python3 tools/bundle_rml_assets.py

# #68: ui_rml/src/rmlui_patch/GeometryBackgroundBorder.cpp is RmlUi's corner
# tessellation with a finer GetNumPoints(); compiled into the binary it preempts
# librmlui.so's copies of that TU. See rmlui_patch/VENDORED.md.
#
# The harness renders through EVO's CPU rasteriser. It used to also link
# RmlUi's GL3 interface for a plane compare against the console's OpenGL path;
# that path is gone (the console is bare-metal AGC only), so the compare had
# nothing left to compare against and went with it.
echo "--- building uiview_playback_rml"
g++ -O2 -std=c++17 \
    -Iprojects/evoplayer \
    -Iprojects/evoplayer/ui_rml/include \
    -Iprojects/evoplayer/ui/include \
    -Iprojects/evoplayer/ui_rml/src \
    -Iprojects/evoplayer/pp/include \
    -Iprojects/evoplayer/include \
    -Ibuild/rmlui-host/RmlUi/Include \
    -Ibuild/rmlui-host-dist/include/freetype2 \
    -o output/uiview/uiview_playback_rml \
    tools/uiview_playback_rml.cpp \
    projects/evoplayer/ui_rml/src/rmlui_patch/GeometryBackgroundBorder.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_render.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_system.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_app.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bridge.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_fileinterface.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bundle.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bundle_data.cpp \
    -Lbuild/rmlui-host-dist/lib \
    -Lbuild/rmlui-host/RmlUi/build \
    -lrmlui -lfreetype -lpng16 -lz -lpthread \
    -Wl,-rpath,/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build

export LD_LIBRARY_PATH="/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build:${LD_LIBRARY_PATH:-}"

shopt -s nullglob
rm -f output/uiview/rml_*.bmp   # stale from a prior run

echo "--- rendering all launch + settings screenshots (CPU rasteriser)"
./output/uiview/uiview_playback_rml

for b in output/uiview/rml_*.bmp; do
    s="$(basename "${b%.bmp}")"
    python3 tools/shot.py png "$b" "output/uiview/$s.png"
    if [[ "${KEEP_BMP:-0}" != "1" ]]; then rm -f "$b"; fi
    echo "  ok -> output/uiview/$s.png"
done

