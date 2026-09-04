#!/usr/bin/env bash
# RmlUi frame profiler — host, no console. See docs/evo-pro/gpu-rendering-plan.md §6.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f /.dockerenv ]]; then
    docker compose run --rm ps5-dev bash ./tools/prof_rmlui.sh
    exit $?
fi

cd "${REPO_ROOT}"

echo "--- building prof_rmlui (-DEVO_RML_PROFILE, -O2)"
g++ -O2 -std=c++17 -DEVO_RML_PROFILE \
    -Iprojects/evoplayer \
    -Iprojects/evoplayer/ui_rml/include \
    -Iprojects/evoplayer/pp/include \
    -Iprojects/evoplayer/include \
    -Ibuild/rmlui-host/RmlUi/Include \
    -Ibuild/rmlui-host-dist/include/freetype2 \
    -o output/uiview/prof_rmlui \
    tools/prof_rmlui.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_render.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_render_agc.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_system.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_app.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bridge.cpp \
    projects/evoplayer/pp/src/pp_agc.c \
    -Lbuild/rmlui-host-dist/lib \
    -Lbuild/rmlui-host/RmlUi/build \
    -lrmlui -lfreetype -lpng16 -lz -lpthread \
    -Wl,-rpath,/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build

export LD_LIBRARY_PATH="/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build:${LD_LIBRARY_PATH:-}"
echo "--- running"
./output/uiview/prof_rmlui
