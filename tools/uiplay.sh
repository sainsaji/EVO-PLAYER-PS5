#!/usr/bin/env bash
# =============================================================================
# tools/uiplay.sh - browse every EVO Player screen in one page, on any machine.
#
#   ./tools/uiplay.sh
#
# Then open output/uiplay/index.html.
#
# What changed (#44)
# ------------------
# This used to be an arrow-key navigable simulation, its frames rendered
# state-by-state by the legacy immediate-mode renderer and stepped through
# with a JS copy of evo_focus / evo_grid. That renderer was retired with the
# RmlUi migration, and the RmlUi host harness renders fixed scenes rather than
# arbitrary (screen, cursor) states, so the faithful nav sim is gone.
#
# What remains is a contact sheet: every screen the RmlUi harness renders
# (tools/uiview_playback_rml.cpp), including the #16 *_stress fixtures, laid
# out in one scrollable page. Click a frame to see it full size. It is the
# same real RmlUi drawing code and assets - just not interactive.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

OUT="${OUTPUT_DIR}/uiplay"
RML_OUT="${OUTPUT_DIR}/uiview"

mkdir -p "${OUT}"
rm -f "${OUT}"/*.png "${OUT}"/index.html

begin "rendering every RmlUi screen"
"$(dirname "${BASH_SOURCE[0]}")/uiview_playback_rml.sh" >/dev/null || die "render failed"

shopt -s nullglob
frames=("${RML_OUT}"/rml_*.png)
(( ${#frames[@]} )) || die "no rml_*.png produced"

for f in "${frames[@]}"; do cp "${f}" "${OUT}/"; done
ok "${#frames[@]} frames -> output/uiplay/"

begin "building the contact sheet"
{
    cat "$(dirname "${BASH_SOURCE[0]}")/uiplay.html"
    echo "<script>const FRAMES = ["
    for f in "${frames[@]}"; do
        b="$(basename "${f}")"
        echo "  \"${b}\","
    done
    echo "];</script>"
    echo "<script>renderGallery();</script>"
} > "${OUT}/index.html"

echo ""
ok "open this in a browser:"
echo "     ${OUT}/index.html"
