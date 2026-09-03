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

# Convert every rml_*.bmp the renderer just emitted. The C++ harness owns the
# screen list now, so this stays correct as fixtures are added.
shopt -s nullglob
for b in output/uiview/rml_*.bmp; do
    s="$(basename "${b%.bmp}")"
    python3 tools/shot.py png "$b" "output/uiview/$s.png"
    rm -f "$b"
    echo "  ok -> output/uiview/$s.png"
done
