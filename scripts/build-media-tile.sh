#!/usr/bin/env bash
# =============================================================================
# scripts/build-media-tile.sh - build the home-screen Media tile launcher.
#
#   ./scripts/build-media-tile.sh                  build both payloads
#   ./scripts/build-media-tile.sh --install        build, then install the tile
#   ./scripts/build-media-tile.sh --uninstall      remove the tile from the PS5
#   ./scripts/build-media-tile.sh --no-build-player   stage the existing player
#
# ---------------------------------------------------------------------------
# WHAT THIS PRODUCES, AND WHY IT IS NOT install-homebrew.sh
#
# install-homebrew.sh puts the player under /data/homebrew and you launch it
# from websrv's index page in a browser on another device. That is the
# development loop and it stays the recommended path.
#
# This builds the other route: a tile in the console's own Media row, so the
# player starts from the controller with no second device. It produces two
# payloads:
#
#   EVOPlayer_MediaLauncher.elf    resident. Registers title EVOP10001,
#                                  serves 127.0.0.1:9056, and launches the
#                                  player when the tile is opened. Must be
#                                  re-injected after every jailbreak.
#   EVOPlayer_UninstallTile.elf    one-shot. Removes EVOP10001 completely.
#
# The launcher EMBEDS the player with .incbin, so the payload is player-sized
# (~34 MB). That is why this script stages output/elf/EVOPlayer.elf into
# assets/ before calling make - the Makefile cannot find it otherwise.
#
# ---------------------------------------------------------------------------
# READ BEFORE --install
#
# This writes to /system_ex (remounted read-write) and registers a title in
# app.db. That is console state, not app state: a half-finished registration
# leaves a ghost tile. Build the uninstaller FIRST - this script always builds
# both, in that order, for exactly that reason - and keep
# EVOPlayer_UninstallTile.elf to hand before installing anything.
#
# EVO Player uses title EVOP10001 and port 9056. ProsperoPlayer uses PRSP10001
# and 9055. They are deliberately different so both tiles can coexist; nothing
# here ever touches upstream's registration. See docs/media-tile.md.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DEST="${REPO_ROOT}/projects/evoplayer/prospero_media_standalone"
PLAYER_ELF="EVOPlayer.elf"
LAUNCHER_ELF="EVOPlayer_MediaLauncher.elf"
UNINSTALL_ELF="EVOPlayer_UninstallTile.elf"

DO_INSTALL=0
DO_UNINSTALL=0
BUILD_PLAYER=1
# -----------------------------------------------------------------------------
# Deploys are bounded, and that is not belt-and-braces.
#
# prospero-deploy is `socat -t 9999999 - TCP:host:9021`. socat returns when the
# far end closes, and elfldr does not close while the payload it spawned is
# alive. The media launcher is RESIDENT by design - it has to be, it is what
# serves the tile's deeplink - so the socket stays open and the deploy never
# returns. Measured behaviour, not theory: it ran until it was killed.
#
# The payload is fully transferred and running long before this expires; the
# timeout only stops us waiting for an EOF that is never coming. 90s is
# generous for ~34 MB over the wire, and exit 124 is treated as success below.
# -----------------------------------------------------------------------------
DEPLOY_TIMEOUT="${EVO_DEPLOY_TIMEOUT:-90}"

while (( $# )); do
    case "$1" in
        --install)   DO_INSTALL=1 ;;
        --uninstall) DO_UNINSTALL=1 ;;
        --no-build-player) BUILD_PLAYER=0 ;;
        --timeout)   shift; DEPLOY_TIMEOUT="${1:?--timeout needs seconds}" ;;
        -h|--help)   sed -n '2,10p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

# Send a payload to elfldr without waiting forever for an EOF that a resident
# payload will never produce. 124 is `timeout`'s own exit code and means the
# payload is up and holding the socket - which is exactly what success looks
# like for the launcher.
deploy_bounded() {
    local elf="$1" what="$2" rc=0

    timeout "${DEPLOY_TIMEOUT}" \
        "${PS5_DEPLOY}" -h "${PS5_HOST}" -p "${PS5_PORT}" "${elf}" || rc=$?

    case "${rc}" in
        0)   ok "${what} sent - payload exited and closed the connection" ;;
        124) ok "${what} sent - still resident after ${DEPLOY_TIMEOUT}s, detached" ;;
        *)   warn "${what}: prospero-deploy exited ${rc}"
             warn "if the console did not react, re-run the jailbreak and retry" ;;
    esac
}

if ! in_container; then
    FWD=()
    (( DO_INSTALL ))   && FWD+=(--install)
    (( DO_UNINSTALL )) && FWD+=(--uninstall)
    reexec_in_container "build-media-tile.sh" "${FWD[@]+"${FWD[@]}"}"
fi

need_file "${DEST}/Makefile" "projects/evoplayer/prospero_media_standalone is missing.
       See docs/media-tile.md"
load_sdk
mkdirs

# -----------------------------------------------------------------------------
# Regenerate the icon if it is absent. It is generated, not hand-drawn, so
# there is no reason for a build to fail on a missing one.
# -----------------------------------------------------------------------------
if [[ ! -f "${DEST}/assets/icon0.png" ]]; then
    begin "generating app icon"
    python3 "${REPO_ROOT}/tools/gen_app_icon.py" >/dev/null \
        || die "tools/gen_app_icon.py failed"
    ok "assets/icon0.png"
fi

# -----------------------------------------------------------------------------
# Stage the player. The launcher embeds it whole, so the tile always runs the
# build that was staged here - not whatever install-homebrew.sh last pushed.
# -----------------------------------------------------------------------------
#
# Build the player rather than trusting whatever is in output/elf.
#
# This used to require a pre-built player and stage whatever it found. Nothing
# checked how old it was, and the version and the in-app changelog live in a
# header and a VERSION file that main.c's make rule does not depend on - so
# after a version bump this staged the PREVIOUS player and embedded it in a
# launcher labelled with the new one. Caught exactly that way: a 0.5.0 tile
# carrying a 0.4.1 player whose changelog stopped at 0.4.0.
#
# The failure is invisible from outside - the tile opens, the player runs, it
# is simply the wrong build - so the fix is to not depend on the caller having
# remembered. build-evoplayer.sh force-relinks for the same reason.
if (( BUILD_PLAYER )); then
    "${REPO_ROOT}/scripts/build-evoplayer.sh"
fi

need_file "${ELF_OUT}/${PLAYER_ELF}" "Build the player first:
       ./scripts/build-evoplayer.sh"
validate_elf "${ELF_OUT}/${PLAYER_ELF}"

begin "staging ${PLAYER_ELF} into the launcher"
cp -f "${ELF_OUT}/${PLAYER_ELF}" "${DEST}/assets/${PLAYER_ELF}"
ok "assets/${PLAYER_ELF} ($(( $(stat -c %s "${DEST}/assets/${PLAYER_ELF}") / 1024 / 1024 )) MiB)"

# -----------------------------------------------------------------------------
# Build. The uninstaller is built first and deliberately: if the launcher build
# fails you still hold a working way to remove a tile from an earlier run.
# -----------------------------------------------------------------------------
begin "building the Media tile payloads"
BUILD_LOG="${LOG_OUT}/media-tile-$(date -u +%Y%m%dT%H%M%SZ).log"

rm -f "${DEST}/${LAUNCHER_ELF}" "${DEST}/${UNINSTALL_ELF}"
if ! make -C "${DEST}" "${UNINSTALL_ELF}" "${LAUNCHER_ELF}" \
        > "${BUILD_LOG}" 2>&1; then
    echo ""
    echo "--- last 40 lines ---"
    tail -40 "${BUILD_LOG}" | sed 's/^/  /'
    die "Media tile build failed. Full log: ${BUILD_LOG#"${REPO_ROOT}/"}"
fi

validate_elf "${DEST}/${LAUNCHER_ELF}"
validate_elf "${DEST}/${UNINSTALL_ELF}"
cp -f "${DEST}/${LAUNCHER_ELF}" "${DEST}/${UNINSTALL_ELF}" "${ELF_OUT}/"

# Prove the identity actually reached the binary. Getting this wrong means
# registering a title under somebody else's id, which is not something to
# discover on the console.
for want in EVOP10001 /data/evoplayer/app; do
    grep -qa -- "${want}" "${ELF_OUT}/${LAUNCHER_ELF}" \
        || die "'${want}' is not present in ${LAUNCHER_ELF}. The identity edit
       did not reach the binary - check prospero_media_launcher.c."
done
# A plain `grep && die` would abort the script on the GOOD path: the AND-list
# returns non-zero when grep finds nothing, and common.sh runs under errexit.
if grep -qa -- "PRSP10001" "${ELF_OUT}/${LAUNCHER_ELF}"; then
    die "${LAUNCHER_ELF} still contains ProsperoPlayer's title id PRSP10001.
       EVO Player must not register upstream's title. Check core/hbldr.c."
fi

ok "-> output/elf/${LAUNCHER_ELF} ($(( $(stat -c %s "${ELF_OUT}/${LAUNCHER_ELF}") / 1024 / 1024 )) MiB)"
ok "-> output/elf/${UNINSTALL_ELF}"

# -----------------------------------------------------------------------------
# Console operations. Both go through elfldr on 9021: these are ordinary
# headless payloads, so SceSpZeroConf is the right place for them - unlike the
# player itself, which needs a display plane.
# -----------------------------------------------------------------------------
if (( DO_UNINSTALL )); then
    require_ps5_host
    begin "removing the EVO Player tile from ${PS5_HOST}"
    deploy_bounded "${ELF_OUT}/${UNINSTALL_ELF}" "uninstaller"
    echo ""
    echo "   Watch for the console toast: 'EVO Player removed from Media'."
    echo "   Verify over FTP that both of these are gone:"
    echo "     /user/app/EVOP10001  and  /system_ex/app/EVOP10001"
    exit 0
fi

if (( DO_INSTALL )); then
    require_ps5_host
    echo ""
    warn "this registers title EVOP10001 in app.db and writes to /system_ex"
    warn "keep output/elf/${UNINSTALL_ELF} - it is the way back"
    echo ""
    begin "installing the tile on ${PS5_HOST}"
    deploy_bounded "${ELF_OUT}/${LAUNCHER_ELF}" "launcher"
    echo ""
    ok "the launcher stays resident and serves 127.0.0.1:9056"
    echo ""
    echo "   On the console:"
    echo "     1. Wait for the toast: 'EVO Player 0.2.0 ready / Open from Media'"
    echo "     2. Home -> Media -> EVO Player"
    echo ""
    echo "   The launcher must stay running. Tapping the tile does nothing if"
    echo "   it is not resident, and it must be re-injected after every"
    echo "   jailbreak - put it on your exploit host's autoload list."
    exit 0
fi

echo ""
echo "   Install the tile:"
echo "     ./scripts/build-media-tile.sh --install"
echo "   Remove it again:"
echo "     ./scripts/build-media-tile.sh --uninstall"
