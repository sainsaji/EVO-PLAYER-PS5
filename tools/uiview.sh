#!/usr/bin/env bash
# =============================================================================
# tools/uiview.sh - render the player's UI on the host and convert to PNG.
#
#   ./tools/uiview.sh --all                every screen
#   ./tools/uiview.sh browse --sel 3
#   ./tools/uiview.sh browse --rail        rail focused (expanded overlay)
#   ./tools/uiview.sh launch --row 2       cursor on the library shelf
#   ./tools/uiview.sh favorites --empty    the empty state
#   ./tools/uiview.sh settings --theme EMBER
#
# Output lands in output/uiview/ as PNGs.
#
# This links the real drawing code against the real font atlas and icons, so
# it is not a mock-up - see the header comment in tools/uiview.c. It exists so
# UI work does not need a console, which matters because every deploy risks a
# kernel panic and costs the better part of an hour to recover from.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

PP="${REPO_ROOT}/projects/evoplayer/pp"
UI="${REPO_ROOT}/projects/evoplayer/ui"
EVO="${REPO_ROOT}/projects/evoplayer"
OUT="${OUTPUT_DIR}/uiview"
BIN="${OUT}/uiview"

need_cmd clang python3
mkdir -p "${OUT}"

begin "building uiview"

# EVO_THEME_DIR points at a directory that will not exist on the host, which is
# the point: evo_theme_init() then finds no .theme files and falls back to the
# four built-ins, so a render is reproducible and does not depend on whatever
# happens to be on someone's USB stick.
clang -O2 -Wall -Wextra -std=gnu11 \
    -DEVO_PLAYER_VERSION='"0.0.2"' \
    -DEVO_THEME_DIR='"/nonexistent/evo_themes"' \
    -I"${PP}/include" -I"${UI}/include" -I"${EVO}" \
    "${REPO_ROOT}/tools/uiview.c" \
    "${UI}/src/evo_draw.c" \
    "${UI}/src/evo_nav.c" \
    "${UI}/src/evo_focus.c" \
    "${UI}/src/evo_chrome.c" \
    "${UI}/src/evo_widgets.c" \
    "${UI}/src/evo_screens.c" \
    "${PP}/src/evo_theme.c" \
    "${PP}/src/evo_ui.c" \
    -lm -o "${BIN}" \
    || die "uiview build failed"

ok "-> output/uiview/uiview"
echo ""

# Only supply the output directory when the caller has not named one. The
# renderer takes the last -o it sees, so appending unconditionally silently
# overrode an explicit path and wrote over a previous render.
HAS_OUT=0
for a in "$@"; do [[ "$a" == "-o" ]] && HAS_OUT=1; done

if (( HAS_OUT )); then
    "${BIN}" "$@" || die "render failed"
else
    "${BIN}" "$@" -o "${OUT}" || die "render failed"
fi

# BMP -> PNG through the same converter the console captures use, so a host
# render and a console capture are directly comparable.
shopt -s nullglob
for b in "${OUT}"/*.bmp; do
    python3 "${REPO_ROOT}/tools/shot.py" png "${b}" "${b%.bmp}.png" 1 >/dev/null
    rm -f "${b}"
done

echo ""
ok "PNGs in output/uiview/"
ls -1 "${OUT}"/*.png 2>/dev/null | sed "s|${REPO_ROOT}/|  |"
