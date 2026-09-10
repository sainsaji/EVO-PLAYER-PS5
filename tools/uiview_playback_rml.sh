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
# render-overhaul GL-2 (#78): the harness also links RmlUi's upstream
# RenderInterface_GL3 (vendored verbatim under ui_rml/src/rmlui_gl3/) plus EVO's
# GL adapters and a headless EGL/llvmpipe context. The CPU rasteriser stays the
# default; UIVIEW_GL=1 runs a second pass with EVO_RML_GL=1 for a plane compare.
echo "--- building uiview_playback_rml"
g++ -O2 -std=c++17 \
    -DEVO_RML_GL_HOST \
    -Iprojects/evoplayer \
    -Iprojects/evoplayer/ui_rml/include \
    -Iprojects/evoplayer/ui_rml/src \
    -Iprojects/evoplayer/pp/include \
    -Iprojects/evoplayer/include \
    -Ibuild/rmlui-host/RmlUi/Include \
    -Ibuild/rmlui-host-dist/include/freetype2 \
    -o output/uiview/uiview_playback_rml \
    tools/uiview_playback_rml.cpp \
    projects/evoplayer/ui_rml/src/rmlui_patch/GeometryBackgroundBorder.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_render.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_render_gl.cpp \
    projects/evoplayer/ui_rml/src/evo_gl_context_host.cpp \
    projects/evoplayer/ui_rml/src/rmlui_gl3/RmlUi_Renderer_GL3.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_system.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_app.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bridge.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_fileinterface.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bundle.cpp \
    projects/evoplayer/ui_rml/src/evo_rmlui_bundle_data.cpp \
    -Lbuild/rmlui-host-dist/lib \
    -Lbuild/rmlui-host/RmlUi/build \
    -lrmlui -lfreetype -lpng16 -lz -lpthread -lEGL -lGL \
    -Wl,-rpath,/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build

# Headless GL: no display, no /dev/dri -> Mesa llvmpipe via the surfaceless
# platform. eglinfo is a cheap sanity probe (mesa-utils-extra).
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export EGL_PLATFORM=surfaceless
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg-uiview}"
mkdir -p "${XDG_RUNTIME_DIR}" 2>/dev/null || true
export LD_LIBRARY_PATH="/workspace/build/rmlui-host-dist/lib:/workspace/build/rmlui-host/RmlUi/build:${LD_LIBRARY_PATH:-}"

shopt -s nullglob
GL_PARITY="${UIVIEW_GL:-0}"
rm -f output/uiview/rml_*.bmp output/uiview/rml_*.cpu.bmp   # stale from a prior run

echo "--- rendering all launch + settings screenshots (CPU rasteriser)"
./output/uiview/uiview_playback_rml

for b in output/uiview/rml_*.bmp; do
    s="$(basename "${b%.bmp}")"
    python3 tools/shot.py png "$b" "output/uiview/$s.png"
    # shot.py diff reads BMP only - keep the CPU BMP for the parity pass.
    if [[ "$GL_PARITY" == "1" ]]; then mv "$b" "output/uiview/${s}.cpu.bmp"; else rm -f "$b"; fi
    echo "  ok -> output/uiview/$s.png"
done

# GL-2 (#78) parity pass: re-render through EvoRenderInterfaceGL and diff every
# screen against the CPU rasteriser. Curved-edge AA is expected to differ - that
# is the improvement, not a regression.
if [[ "$GL_PARITY" == "1" ]]; then
    echo "--- rendering through the OpenGL render interface (EVO_RML_GL=1)"
    eglinfo -B 2>&1 | sed -n '1,10p' || echo "  (eglinfo unavailable)"
    EVO_RML_GL=1 ./output/uiview/uiview_playback_rml
    echo "--- CPU vs GL plane compare (threshold 8)"
    for glb in output/uiview/rml_*.bmp; do
        [[ "$glb" == *.cpu.bmp ]] && continue
        s="$(basename "${glb%.bmp}")"
        cpub="output/uiview/${s}.cpu.bmp"
        if [[ -f "$cpub" ]]; then
            printf '  %-30s ' "$s"
            python3 tools/shot.py diff "$cpub" "$glb"
        fi
        python3 tools/shot.py png "$glb" "output/uiview/${s}_gl.png"
        rm -f "$glb"
    done
    rm -f output/uiview/rml_*.cpu.bmp
fi
