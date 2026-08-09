#!/usr/bin/env bash
# =============================================================================
# tools/uiplay.sh - a navigable EVO Player UI, in a browser, on any machine.
#
#   ./tools/uiplay.sh                 build it
#   ./tools/uiplay.sh --theme EMBER   in a different theme
#
# Then open output/uiplay/index.html.
#
# How it works
# ------------
# There is no emscripten, no mingw and no host SDL2 in the container, so there
# is no way to build a windowed binary for Windows from here. But the frames
# do not have to be produced live: this repo owns both the renderer and the
# navigation model, so every state the cursor can actually reach can be
# rendered ahead of time and stepped through with the same rules the player
# uses.
#
# The pictures are therefore real - the same SDF primitives, font atlas and
# icons the console draws with - and the navigation is a faithful copy of
# evo_focus / evo_grid / evo_nav rather than a guess at them.
#
# What it is not: live data, animation, or the player's timing. It shows
# layout and navigation, which is what cannot otherwise be checked without a
# console.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

OUT="${OUTPUT_DIR}/uiplay"
BIN="${OUTPUT_DIR}/uiview/uiview"
THEME=""

while (( $# )); do
    case "$1" in
        --theme) shift; THEME="${1:?--theme needs a name}" ;;
        -h|--help) sed -n '3,8p' "$0"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
    shift
done

need_cmd python3
mkdir -p "${OUT}"
rm -f "${OUT}"/*.png "${OUT}"/*.bmp

# Build the renderer via uiview.sh so there is one build path, not two.
begin "building renderer"
"${REPO_ROOT}/tools/uiview.sh" launch -o "${OUTDIR:-${OUTPUT_DIR}/uiview}" \
    >/dev/null 2>&1 || die "renderer build failed"
[[ -x "${BIN}" ]] || die "renderer missing at ${BIN}"

THEME_ARG=()
[[ -n "${THEME}" ]] && THEME_ARG=(--theme "${THEME}")

render() {           # render <name> <args...>
    local name="$1"; shift
    "${BIN}" "$@" "${THEME_ARG[@]}" -o "${OUT}/${name}.bmp" >/dev/null \
        || die "render ${name} failed"
}

begin "rendering states"

# -- launch: one state per cursor position -----------------------------------
# Row 0 is the hero (one column), row 1 the recent shelf, row 2 the library.
render "launch_r0_c0" launch --row 0
for c in 0 1 2 3 4 5 6; do render "launch_r1_c${c}" launch --row 1 --sel "${c}"; done
for c in 0 1 2 3 4 5;   do render "launch_r2_c${c}" launch --row 2 --sel "${c}"; done

# -- section screens: one state per row ---------------------------------------
for s in 0 1 2 3 4 5 6 7 8 9; do render "browse_s${s}" browse --sel "${s}"; done
for s in 0 1 2 3 4;           do render "recent_s${s}" recent --sel "${s}"; done
for s in 0 1;                 do render "favorites_s${s}" favorites --sel "${s}"; done
for s in 0 1 2 3 4 5 6 7;     do render "settings_s${s}" settings --sel "${s}"; done
for s in 0 1 2 3;             do render "tools_s${s}" tools --sel "${s}"; done
for s in 0 1 2 3 4;           do render "about_s${s}" about --sel "${s}"; done

# -- the rail overlay ---------------------------------------------------------
# Rendered once per section per rail position. The content behind is dimmed
# heavily enough that using the section's first row for all of them is not
# noticeable, which keeps this from multiplying by every selection.
for scr in browse recent favorites settings tools about; do
    for r in 0 1 2 3 4 5 6; do
        render "${scr}_rail${r}" "${scr}" --sel 0 --rail --rail-sel "${r}"
    done
done

# -- empty states -------------------------------------------------------------
render "recent_empty"    recent    --empty
render "favorites_empty" favorites --empty
render "browse_empty"    browse    --empty

begin "converting to PNG"
shopt -s nullglob
n=0
for b in "${OUT}"/*.bmp; do
    python3 "${REPO_ROOT}/tools/shot.py" png "${b}" "${b%.bmp}.png" 2 >/dev/null
    rm -f "${b}"
    n=$(( n + 1 ))
done
ok "${n} frames"

cp "${REPO_ROOT}/tools/uiplay.html" "${OUT}/index.html"

echo ""
ok "open this in a browser:"
echo "     ${OUT}/index.html"
echo ""
echo "   Arrow keys move, Enter selects, Esc/Backspace goes back."
echo "   Left from a list opens the navigation rail, as on the console."
