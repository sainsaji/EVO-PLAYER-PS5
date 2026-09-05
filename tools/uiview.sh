#!/usr/bin/env bash
# =============================================================================
# tools/uiview.sh - render the player's UI on the host and convert to PNG.
#
#   ./tools/uiview.sh --all           every screen
#
# Since the RmlUi migration (#44) this is a thin wrapper around
# tools/uiview_playback_rml.sh: that harness links the real RmlUi documents,
# stylesheets, fonts and icons and renders every screen (plus a *_stress
# fixture per #16 screen) to output/uiview/rml_*.png. The legacy immediate-mode
# renderer and its per-screen CLI were retired with ui/src/evo_screens.c.
#
# For a specific screen, or a different cursor position, add a fixture to
# tools/uiview_playback_rml.cpp - the C++ harness owns the screen list now.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

case "${1:-}" in
    ""|--all|-a) ;;
    -h|--help) sed -n '3,16p' "$0"; exit 0 ;;
    *)
        echo "uiview.sh: per-screen selection was removed with the legacy"
        echo "renderer (#44). Rendering the full RmlUi set instead."
        echo ""
        ;;
esac

exec "$(dirname "${BASH_SOURCE[0]}")/uiview_playback_rml.sh"
