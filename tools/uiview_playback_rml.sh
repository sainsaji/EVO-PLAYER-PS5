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

echo "--- building uiview_playback_rml"
g++ -O2 -std=c++17 \
    -Iprojects/evoplayer \
    -Iprojects/evoplayer/ui_rml/include \
    -Iprojects/evoplayer/pp/include \
    -Iprojects/evoplayer/include \
    -Ibuild/rmlui-host/RmlUi/Include \
    -Ibuild/rmlui-host-dist/include/freetype2 \
    -o output/uiview/uiview_playback_rml \
    tools/uiview_playback_rml.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_render.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_render_agc.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_system.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_app.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bridge.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_fileinterface.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bundle.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bundle_data.cpp \
    projects/evoplayer/pp/src/pp_agc.c \
    -Lbuild/rmlui-host-dist/lib \
    -Lbuild/rmlui-host/RmlUi/build \
    -lrmlui -lfreetype -lpng16 -lz -lpthread \
    -Wl,-rpath,/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build

echo "--- rendering all launch + settings screenshots"
export LD_LIBRARY_PATH="/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build:${LD_LIBRARY_PATH:-}"
./output/uiview/uiview_playback_rml

# Convert every rml_*.bmp the renderer just emitted. The C++ harness owns the
# screen list now, so this stays correct as fixtures are added.
shopt -s nullglob
for b in output/uiview/rml_*.bmp; do
    s="$(basename "${b%.bmp}")"
    python3 tools/shot.py png "$b" "output/uiview/$s.png"
    rm -f "$b"
    echo "  ok -> output/uiview/$s.png"
done
