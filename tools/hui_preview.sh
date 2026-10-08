#!/usr/bin/env bash
# =============================================================================
# tools/hui_preview.sh - render EVO's ps5-homebrew-ui screens on the host.
#
#   ./tools/hui_preview.sh [width height]     -> output/uiview/hui_*.png
#
# Builds tools/hui_preview/hui_preview.cpp with the kit's OpenGL backend and
# EVO's screen code (projects/evoplayer/ui_kit), then renders every fixture
# through Mesa's surfaceless EGL. Same screens and shader as the console; the
# console draws them through sceAgc instead (ui_kit/src/hui_agc_batch.cpp).
# Re-runs itself in the dev container when started from Windows.
# =============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ! -f /.dockerenv ]]; then
    cd "${ROOT}"
    MSYS_NO_PATHCONV=1 exec docker compose run --rm ps5-dev bash ./tools/hui_preview.sh "$@"
fi

KIT="${ROOT}/third_party/ps5-homebrew-ui"
EVO="${ROOT}/projects/evoplayer"
BUILD="${ROOT}/build/hui_preview"
OUT="${ROOT}/output/uiview"
mkdir -p "${BUILD}/obj" "${OUT}"

CXX="$(command -v clang++-18 || command -v g++)"
FLAGS=(-std=c++20 -O2 -w -DGL_GLEXT_PROTOTYPES=1
       -I"${KIT}/src" -I"${KIT}" -I"${KIT}/host"
       -I"${EVO}/ui_kit/include" -I"${EVO}/ui_rml/include" -I"${EVO}/include")

SOURCES=("${ROOT}/tools/hui_preview/hui_preview.cpp"
         "${EVO}/ui_kit/src/evo_hui_screens.cpp"
         "${EVO}/ui_kit/src/evo_hui_browser.cpp"
         "${EVO}/ui_kit/src/evo_hui_settings.cpp"
         "${EVO}/ui_kit/src/evo_hui_osd.cpp"
         "${EVO}/ui_kit/src/evo_hui_modals.cpp"
         "${EVO}/ui_kit/src/evo_hui_misc.cpp"
         "${EVO}/ui_kit/src/evo_hui_extra.cpp"
         "${KIT}/host/platform_host.cpp")
while IFS= read -r -d '' f; do SOURCES+=("$f"); done < <(
    find "${KIT}/src/gfx" "${KIT}/src/ui" "${KIT}/src/core" -name '*.cpp' -print0 | sort -z)
SOURCES+=("${KIT}/src/audio/cues.cpp" "${KIT}/src/audio/mixer.cpp" "${KIT}/src/audio/wav.cpp")

# Compile what changed, in parallel; headers are not tracked, so touch a
# source (or delete build/hui_preview) after changing one.
OBJS=()
TODO=()
for src in "${SOURCES[@]}"; do
    rel="${src#"${ROOT}/"}"
    obj="${BUILD}/obj/${rel//\//_}.o"
    OBJS+=("${obj}")
    newest="${src}"
    [[ "${src}" == "${EVO}"/* || "${src}" == "${ROOT}/tools/"* ]] && \
        newest="$(ls -t "${src}" "${EVO}"/ui_kit/include/*.hpp | head -1)"
    if [[ ! -f "${obj}" || "${newest}" -nt "${obj}" ]]; then
        TODO+=("${src}|${obj}")
    fi
done
if (( ${#TODO[@]} )); then
    printf '%s\n' "${TODO[@]}" | xargs -P "$(nproc)" -I{} bash -c \
        'src="${1%%|*}"; obj="${1##*|}"; shift; "$@" -c "$src" -o "$obj" || exit 255' \
        _ {} "${CXX}" "${FLAGS[@]}"
fi
"${CXX}" "${OBJS[@]}" -lEGL -lGL -lm -o "${BUILD}/hui_preview"
"${BUILD}/hui_preview" "${ROOT}" "${OUT}" "$@"
