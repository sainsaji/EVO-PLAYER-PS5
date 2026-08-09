#!/usr/bin/env bash
# =============================================================================
# tools/shot.sh - get screenshots off the console and look at them properly.
#
#   ./tools/shot.sh grab              newest capture -> output/screenshots/latest.png
#   ./tools/shot.sh grab evo_shot_003.bmp
#   ./tools/shot.sh grab --full       full resolution instead of half
#   ./tools/shot.sh list              what is on the stick
#   ./tools/shot.sh clean             delete the BMPs from the stick
#   ./tools/shot.sh probe 180 214     colour at a framebuffer coordinate
#   ./tools/shot.sh probe 180 214 1560 96      summary of a region
#   ./tools/shot.sh scan row 260      pixel runs along a line
#   ./tools/shot.sh crop 1228 222 560 315      cut a region out at full size
#   ./tools/shot.sh diff a.bmp b.bmp  what changed between two captures
#
# The player writes captures when built with -DEVO_AUTOSHOT=N (capture N
# seconds after launch) and on L3/R3 in the menus. See docs/tooling.md.
#
# probe/scan/crop default to the most recently grabbed BMP, so the usual loop
# is one `grab` followed by as many measurements as the question needs.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

SHOT_DIR="${OUTPUT_DIR}/screenshots"
SHOT_PY="${REPO_ROOT}/tools/shot.py"
WEB_PORT="${PS5_WEB_PORT:-8080}"
USB="/mnt/usb0"

mkdir -p "${SHOT_DIR}"

# Remember which BMP was grabbed last so the measurement commands need no
# arguments. Without this every probe repeats a filename nobody has memorised.
LAST_MARKER="${SHOT_DIR}/.last"

usb_listing() {
    require_ps5_host
    curl -s --connect-timeout 8 "http://${PS5_HOST}:${WEB_PORT}/fs${USB}/?fmt=json"
}

newest_remote() {
    usb_listing | tr ',' '\n' | grep -oE 'evo_shot_[0-9]+\.bmp' | sort -u | tail -1
}

resolve_local() {
    # $1 may be a filename, a path, or empty (meaning "the last one grabbed").
    local want="${1:-}"

    if [[ -z "${want}" ]]; then
        [[ -f "${LAST_MARKER}" ]] \
            || die "no capture grabbed yet in this checkout - run: ./tools/shot.sh grab"
        want="$(cat "${LAST_MARKER}")"
    fi

    [[ -f "${want}" ]] && { echo "${want}"; return; }
    [[ -f "${SHOT_DIR}/${want}" ]] && { echo "${SHOT_DIR}/${want}"; return; }

    die "no such capture: ${want}"
}

cmd_grab() {
    local name="" scale=2

    while (( $# )); do
        case "$1" in
            --full) scale=1 ;;
            --half) scale=2 ;;
            *)      name="$1" ;;
        esac
        shift
    done

    require_ps5_host
    need_cmd curl python3

    if [[ -z "${name}" ]]; then
        begin "finding the newest capture on ${USB}"
        name="$(newest_remote || true)"
        [[ -n "${name}" ]] || die "no evo_shot_*.bmp on the stick.

       Captures are written by a build with -DEVO_AUTOSHOT=N, or by
       pressing L3/R3 while in the menus. Rebuild with:
           EXTRA_CFLAGS=\"-DEVO_AUTOSHOT=4\" ./scripts/build-evoplayer.sh --run"
    fi

    begin "fetching ${name}"
    curl -s --connect-timeout 15 --max-time 180 \
         -o "${SHOT_DIR}/${name}" \
         "http://${PS5_HOST}:${WEB_PORT}/fs${USB}/${name}" \
        || die "fetch failed - is ps5-payload-websrv running?"

    [[ -s "${SHOT_DIR}/${name}" ]] || die "${name} came back empty"

    echo "${SHOT_DIR}/${name}" > "${LAST_MARKER}"

    python3 "${SHOT_PY}" png \
        "${SHOT_DIR}/${name}" "${SHOT_DIR}/latest.png" "${scale}"

    ok "-> output/screenshots/latest.png"
    echo "   source: output/screenshots/${name}"
}

cmd_list() {
    require_ps5_host
    local shots
    shots="$(usb_listing | tr ',' '\n' | grep -oE 'evo_shot_[0-9]+\.bmp' | sort -u || true)"

    if [[ -z "${shots}" ]]; then
        log "no captures on the stick"
    else
        log "captures on ${USB}"
        echo "${shots}" | sed 's/^/  /'
        # Each of these is ~6MB of uncompressed framebuffer and they are never
        # cleaned up by the player itself.
        echo ""
        echo "   $(echo "${shots}" | wc -l) file(s), roughly $(( $(echo "${shots}" | wc -l) * 6 ))MB"
        echo "   remove them with: ./tools/shot.sh clean"
    fi

    if compgen -G "${SHOT_DIR}/evo_shot_*.bmp" > /dev/null; then
        echo ""
        log "already fetched locally"
        ls -1 "${SHOT_DIR}"/evo_shot_*.bmp | xargs -n1 basename | sed 's/^/  /'
    fi
}

# The websrv /fs endpoint is read-only - it serves files and JSON listings and
# implements no DELETE (checked against its own apiClient.js, which only ever
# GETs). So this reports what is there and how to remove it rather than
# pretending to do it.
cmd_clean() {
    require_ps5_host

    local shots count
    shots="$(usb_listing | tr ',' '\n' | grep -oE 'evo_shot_[0-9]+\.bmp' | sort -u || true)"
    [[ -n "${shots}" ]] || { log "nothing on the stick to clean"; return; }

    count="$(echo "${shots}" | wc -l)"

    warn "the console's web server serves ${USB} read-only - it has no DELETE."
    echo ""
    echo "   ${count} capture(s) on the stick, roughly $(( count * 6 ))MB:"
    echo "${shots}" | sed 's/^/     /'
    echo ""
    echo "   Remove them with whichever you have to hand:"
    echo "     * the terminal in http://${PS5_HOST}:${WEB_PORT}/  ->  rm ${USB}/evo_shot_*.bmp"
    echo "     * take the stick out and delete them on a PC"
    echo ""
    echo "   Local copies under output/screenshots are yours to delete freely."
}

cmd_probe() {
    need_cmd python3
    local src; src="$(resolve_local "${SHOT_SRC:-}")"
    python3 "${SHOT_PY}" probe "${src}" "$@"
}

cmd_scan() {
    need_cmd python3
    local src; src="$(resolve_local "${SHOT_SRC:-}")"
    python3 "${SHOT_PY}" scan "${src}" "$@"
}

cmd_crop() {
    need_cmd python3
    local src; src="$(resolve_local "${SHOT_SRC:-}")"
    local out="${SHOT_DIR}/crop.png"
    python3 "${SHOT_PY}" crop "${src}" "${out}" "$@"
    ok "-> output/screenshots/crop.png"
}

cmd_diff() {
    need_cmd python3
    [[ $# -ge 2 ]] || die "diff needs two captures"
    local a b
    a="$(resolve_local "$1")"; b="$(resolve_local "$2")"; shift 2
    python3 "${SHOT_PY}" diff "${a}" "${b}" "$@"
}

CMD="${1:-grab}"
shift || true

case "${CMD}" in
    grab)  cmd_grab "$@" ;;
    list)  cmd_list "$@" ;;
    clean) cmd_clean "$@" ;;
    probe) cmd_probe "$@" ;;
    scan)  cmd_scan "$@" ;;
    crop)  cmd_crop "$@" ;;
    diff)  cmd_diff "$@" ;;
    -h|--help|help) sed -n '3,22p' "$0" ;;
    *) die "unknown command: ${CMD} (try --help)" ;;
esac
