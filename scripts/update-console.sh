#!/usr/bin/env bash
# =============================================================================
# scripts/update-console.sh - put the current build on the console.
#
#   ./scripts/update-console.sh              rebuild and update both installs
#   ./scripts/update-console.sh --tile       the home-screen tile only
#   ./scripts/update-console.sh --homebrew   the websrv install only
#   ./scripts/update-console.sh --no-build   deploy what is already built
#
# ---------------------------------------------------------------------------
# WHY THIS EXISTS - the trap it removes
#
# The player exists on the console TWICE, and the two are updated by different
# commands:
#
#   /data/homebrew/EVOPlayer/eboot.elf   websrv launcher   install-homebrew.sh
#   /data/evoplayer/app/eboot.elf        Media tile        build-media-tile.sh
#
# The tile's copy is EMBEDDED inside EVOPlayer_MediaLauncher.elf and rewritten
# from that embedded copy on every launch. So updating the homebrew install
# does not update the tile: you rebuild, press the tile, and get the previous
# build with nothing on screen to say why. That is a genuinely confusing
# afternoon, and it is the whole reason this script defaults to updating both.
#
# Neither path launches anything. Starting the player is a separate, deliberate
# act - a second instance in the app slot is what kernel-panicked a console
# during development.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DO_TILE=1
DO_HOMEBREW=1
DO_BUILD=1
# Bounds the tile deploy. The launcher is resident, so elfldr never closes the
# socket and an unbounded deploy runs until it is killed - see the long comment
# in build-media-tile.sh.
DEPLOY_TIMEOUT="${EVO_DEPLOY_TIMEOUT:-90}"

while (( $# )); do
    case "$1" in
        --tile)     DO_HOMEBREW=0 ;;
        --homebrew) DO_TILE=0 ;;
        --no-build) DO_BUILD=0 ;;
        --timeout)  shift; DEPLOY_TIMEOUT="${1:?--timeout needs seconds}" ;;
        -h|--help)  sed -n '2,9p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

if ! in_container; then
    FWD=()
    (( DO_HOMEBREW )) || FWD+=(--tile)
    (( DO_TILE ))     || FWD+=(--homebrew)
    (( DO_BUILD ))    || FWD+=(--no-build)
    FWD+=(--timeout "${DEPLOY_TIMEOUT}")
    reexec_in_container "update-console.sh" "${FWD[@]+"${FWD[@]}"}"
fi

require_ps5_host
mkdirs

VERSION="$(tr -d '[:space:]' < "${REPO_ROOT}/projects/evoplayer/VERSION")"

echo ""
echo "   EVO Player ${VERSION}  ->  ${PS5_HOST}"
echo "   tile: $(( DO_TILE ))   homebrew: $(( DO_HOMEBREW ))   build: $(( DO_BUILD ))"
echo ""

# -----------------------------------------------------------------------------
# Build once, deploy twice. Both install paths take the same ELF, so building
# per-path would be the same work done twice and could - if a source file were
# touched in between - ship two different binaries under one version number.
# -----------------------------------------------------------------------------
if (( DO_BUILD )); then
    "${SCRIPTS_DIR}/build-evoplayer.sh"
else
    need_file "${ELF_OUT}/EVOPlayer.elf" "Nothing built yet. Drop --no-build."
fi

validate_elf "${ELF_OUT}/EVOPlayer.elf"

if (( DO_HOMEBREW )); then
    echo ""
    "${SCRIPTS_DIR}/install-homebrew.sh" --name EVOPlayer \
        "${ELF_OUT}/EVOPlayer.elf"
fi

if (( DO_TILE )); then
    echo ""
    # --install re-registers the title as well as re-injecting the launcher.
    # Both are idempotent: the title id does not change, and a resident
    # launcher hands over port 9056 when a new one starts.
    "${SCRIPTS_DIR}/build-media-tile.sh" --install \
        --timeout "${DEPLOY_TIMEOUT}"
fi

echo ""
ok "console is on ${VERSION}"
echo ""
if (( DO_TILE )); then
    echo "   Tile:     Home -> Media -> EVO Player"
fi
if (( DO_HOMEBREW )); then
    echo "   Browser:  http://${PS5_HOST}:8080/index.html"
fi
echo ""
echo "   Exit the app on the console before launching again - the app slot"
echo "   stays resident, and stacking instances has panicked a console."
