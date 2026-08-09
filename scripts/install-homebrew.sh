#!/usr/bin/env bash
# =============================================================================
# scripts/install-homebrew.sh - install a payload as launchable PS5 homebrew.
#
#   ./scripts/install-homebrew.sh --setup                 deploy the daemons
#   ./scripts/install-homebrew.sh output/elf/videoout_test.elf
#   ./scripts/install-homebrew.sh --run output/elf/videoout_test.elf
#   ./scripts/install-homebrew.sh --run --timeout 90 output/elf/evoplayer.elf
#   ./scripts/install-homebrew.sh --list                  what is installed
#
# --run installs and then launches it, streaming the payload's stdout back.
# This is the fast edit-build-see-it loop for anything graphical.
#
# ---------------------------------------------------------------------------
# WHY THIS EXISTS - the thing that is not obvious
#
# deploy.sh sends an ELF to ps5-payload-elfldr on 9021, which spawns it inside
# SceSpZeroConf:
#
#     websrv/src/ps5/elfldr.c:74
#       static const char* SceSpZeroConf = "/system/vsh/app/NPXS40112/eboot.bin";
#
# SceSpZeroConf is a background network-discovery service. It has no display
# plane, no audio output, and no graphics memory budget (klog shows the payload
# spawned with `dmem#0`). VideoOut and AudioOut calls all SUCCEED there - the
# handles are valid and flips are accepted - but nothing reaches the TV or the
# speakers. Measured on 12.70: videoout_test reported 960 clean flips with a
# blank screen.
#
# Graphical homebrew has to run in a foreground app instead:
#
#     websrv/src/ps5/hbldr.c:45
#       #define PSNOW_EBOOT "/system_ex/app/NPXS40106/eboot.bin"
#       ... sceSystemServiceLaunchApp("FAKE00000", argv, &ctx);
#
# ps5-payload-websrv borrows the PlayStation Now app slot, which DOES have a
# display plane and a real memory budget. Note that POSTing an ELF to
# websrv's /elfldr endpoint does NOT do this - that path calls elfldr_spawn and
# lands back in SceSpZeroConf. Only the homebrew launcher goes through
# hbldr_launch.
#
# So: anything that draws or plays sound must be installed as homebrew and
# launched from websrv's index page. That is what this script automates.
# ---------------------------------------------------------------------------
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

WEBSRV_VERSION="v0.34"
FTPSRV_VERSION="v0.21"
WEBSRV_PORT=8080
FTPSRV_PORT=2121
HB_DIR="/data/homebrew"

DO_SETUP=0
DO_LIST=0
DO_RUN=0
ELF=""
NAME=""
# Bounded so --run can never hang: the /hbldr log pipe stays open after the
# payload exits. videoout_test needs ~16 s, so 45 s is a comfortable default.
RUN_TIMEOUT=45
# Arguments passed to the payload. hbldr splits this into argv, so argv[0] is
# the FIRST word - include a placeholder if your payload reads argv[1], e.g.
#   --args "eboot.elf pattern"
RUN_ARGS=""

while (( $# )); do
    case "$1" in
        --setup)  DO_SETUP=1 ;;
        --list)   DO_LIST=1 ;;
        --run)    DO_RUN=1 ;;
        --timeout) shift; RUN_TIMEOUT="${1:?--timeout needs seconds}" ;;
        --args)    shift; RUN_ARGS="${1?--args needs a value}" ;;
        --name)   shift; NAME="${1:?--name needs a value}" ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        -*)       die "unknown option: $1 (try --help)" ;;
        *)        ELF="$1" ;;
    esac
    shift
done

if ! in_container; then
    FWD=()
    (( DO_SETUP )) && FWD+=(--setup)
    (( DO_LIST ))  && FWD+=(--list)
    (( DO_RUN ))   && FWD+=(--run --timeout "${RUN_TIMEOUT}")
    [[ -n "${RUN_ARGS}" ]] && FWD+=(--args "${RUN_ARGS}")
    [[ -n "${NAME}" ]] && FWD+=(--name "${NAME}")
    [[ -n "${ELF}" ]]  && FWD+=("${ELF}")
    reexec_in_container "install-homebrew.sh" "${FWD[@]+"${FWD[@]}"}"
fi

require_ps5_host
need_cmd curl nc
mkdirs

FTP_URL="ftp://${PS5_HOST}:${FTPSRV_PORT}"

# -----------------------------------------------------------------------------
# --setup: push the two daemons through elfldr. They are ordinary payloads, so
# they are happy in SceSpZeroConf - neither draws anything.
# -----------------------------------------------------------------------------
if (( DO_SETUP )); then
    load_sdk
    CACHE="${REPO_ROOT}/third_party/daemons"
    mkdir -p "${CACHE}"

    fetch_and_deploy() {
        local name="$1" ver="$2" asset="$3" port="$4"
        local elf="${CACHE}/${name}-${ver}.elf"

        if [[ ! -f "${elf}" ]]; then
            begin "downloading ${name} ${ver}"
            wget -q -O "${elf}.part" \
              "https://github.com/ps5-payload-dev/${name}/releases/download/${ver}/${asset}" \
              || die "download failed for ${name} ${ver}"
            mv "${elf}.part" "${elf}"
        fi
        validate_elf "${elf}"

        if nc -z -w 3 "${PS5_HOST}" "${port}" 2>/dev/null; then
            ok "${name} already listening on ${port}"
            return 0
        fi

        begin "deploying ${name} -> ${PS5_HOST}:${PS5_PORT}"
        "${PS5_DEPLOY}" -h "${PS5_HOST}" -p "${PS5_PORT}" "${elf}" || true

        # Daemons take a moment to bind.
        for _ in $(seq 1 10); do
            if nc -z -w 2 "${PS5_HOST}" "${port}" 2>/dev/null; then
                ok "${name} is listening on ${port}"
                return 0
            fi
        done
        die "${name} did not start listening on ${port}.
       Re-run the jailbreak and make sure elfldr is still running."
    }

    fetch_and_deploy websrv "${WEBSRV_VERSION}" websrv-ps5.elf "${WEBSRV_PORT}"
    fetch_and_deploy ftpsrv "${FTPSRV_VERSION}" ftpsrv-ps5.elf "${FTPSRV_PORT}"

    echo ""
    ok "daemons ready"
    echo "   websrv : http://${PS5_HOST}:${WEBSRV_PORT}/index.html"
    echo "   ftpsrv : ${FTP_URL}"
    echo ""
    echo "   Now install a payload:"
    echo "     ./scripts/install-homebrew.sh output/elf/videoout_test.elf"
    exit 0
fi

# -----------------------------------------------------------------------------
# --list
# -----------------------------------------------------------------------------
if (( DO_LIST )); then
    begin "installed homebrew on ${PS5_HOST}"
    curl -s --connect-timeout 5 "${FTP_URL}${HB_DIR}/" \
      || die "cannot list ${FTP_URL}${HB_DIR}/ - is ftpsrv running?
       Run: ./scripts/install-homebrew.sh --setup"
    exit 0
fi

# -----------------------------------------------------------------------------
# Install a payload as homebrew.
# -----------------------------------------------------------------------------
[[ -n "${ELF}" ]] || die "no ELF given.
       Usage: $(basename "$0") [--setup] <payload.elf>"

if [[ ! -f "${ELF}" ]]; then
    for cand in "${REPO_ROOT}/${ELF}" "${ELF_OUT}/${ELF}" "${ELF_OUT}/${ELF}.elf"; do
        [[ -f "${cand}" ]] && { ELF="${cand}"; break; }
    done
fi
need_file "${ELF}" "Build it first: ./scripts/build.sh"
validate_elf "${ELF}"

[[ -n "${NAME}" ]] || NAME="$(basename "${ELF}" .elf)"

begin "checking daemons"
nc -z -w 3 "${PS5_HOST}" "${FTPSRV_PORT}" 2>/dev/null \
  || die "ftpsrv is not listening on ${PS5_HOST}:${FTPSRV_PORT}.
       Run once per jailbreak:  ./scripts/install-homebrew.sh --setup"
ok "ftpsrv on ${FTPSRV_PORT}"
nc -z -w 3 "${PS5_HOST}" "${WEBSRV_PORT}" 2>/dev/null \
  && ok "websrv on ${WEBSRV_PORT}" \
  || warn "websrv is not on ${WEBSRV_PORT} - you will not be able to launch it"

# Build the bundle websrv expects: eboot.elf + sce_sys/icon0.png.
begin "packaging ${NAME}"
"${SCRIPTS_DIR}/package-pkg.sh" --format homebrew --title "${NAME}" "${ELF}" \
    >/dev/null || die "packaging failed"
BUNDLE="${PKG_OUT}/${NAME}"
need_file "${BUNDLE}/eboot.elf"

begin "uploading to ${HB_DIR}/${NAME}/"
# ftpsrv has no recursive mkdir, so create each level and ignore "exists".
for d in "${HB_DIR}" "${HB_DIR}/${NAME}" "${HB_DIR}/${NAME}/sce_sys"; do
    curl -s -Q "MKD ${d}" "${FTP_URL}/" >/dev/null 2>&1 || true
done

upload() {
    local src="$1" dst="$2"
    curl -s --connect-timeout 10 -T "${src}" "${FTP_URL}${dst}" \
      || die "upload failed: ${dst}
       Is /data writable? Try: curl -Q 'MTRW' ${FTP_URL}/"
    printf '  uploaded %-24s %s KiB\n' "$(basename "${dst}")" \
           "$(( $(stat -c %s "${src}") / 1024 ))"
}

upload "${BUNDLE}/eboot.elf"            "${HB_DIR}/${NAME}/eboot.elf"
upload "${BUNDLE}/sce_sys/icon0.png"    "${HB_DIR}/${NAME}/sce_sys/icon0.png"
[[ -f "${BUNDLE}/homebrew.js" ]] && \
    upload "${BUNDLE}/homebrew.js"      "${HB_DIR}/${NAME}/homebrew.js"

echo ""
ok "installed as '${NAME}'"

# -----------------------------------------------------------------------------
# --run: launch through websrv's /hbldr endpoint.
#
# GET /hbldr?path=...&pipe=1 calls sys_launch_homebrew -> hbldr_launch, i.e.
# the PS Now app slot WITH a display plane, and streams the payload's stdout
# back as text/x-log. That is what makes this a usable dev loop: build,
# install, launch and read the output without touching the console.
#
# Do NOT confuse this with POSTing to /elfldr - that path calls elfldr_spawn
# and lands in headless SceSpZeroConf, where nothing is drawn.
# -----------------------------------------------------------------------------
if (( DO_RUN )); then
    begin "launching ${NAME} (graphical app slot, timeout ${RUN_TIMEOUT}s)"
    echo ""

    # The /hbldr pipe does NOT close when the payload exits - the app slot
    # process stays alive - so curl would hang forever waiting for EOF. Bound
    # it with --max-time and treat curl's timeout (exit 28) as success, since
    # by then we have already streamed the payload's output.
    # `cmd || crc=$?` rather than bracketing with `set +e`: bash runs the ERR
    # trap even when errexit is off, but the left side of `||` is exempt.
    crc=0
    curl -sS --max-time "${RUN_TIMEOUT}" --get \
         --data-urlencode "path=${HB_DIR}/${NAME}/eboot.elf" \
         --data-urlencode "args=${RUN_ARGS}" \
         --data-urlencode "pipe=1" \
         "http://${PS5_HOST}:${WEBSRV_PORT}/hbldr" 2>/dev/null || crc=$?

    echo ""
    case "${crc}" in
        0)  ok "${NAME} finished (stream closed)" ;;
        28) ok "${NAME} launched; detached after ${RUN_TIMEOUT}s"
            echo "   The app slot stays resident, so the log pipe never EOFs."
            echo "   Raise the limit with --timeout N if your payload runs longer." ;;
        7)  die "could not connect to websrv on ${PS5_HOST}:${WEBSRV_PORT}.
       Re-run: ./scripts/install-homebrew.sh --setup" ;;
        *)  die "launch failed (curl exit ${crc})." ;;
    esac
else
    echo ""
    echo "   Launch it:"
    echo "     ./scripts/install-homebrew.sh --run ${ELF#"${REPO_ROOT}/"}"
    echo "   or open http://${PS5_HOST}:${WEBSRV_PORT}/index.html and click ${NAME}"
    echo ""
    echo "   This runs in the PS Now app slot via hbldr_launch, which has a real"
    echo "   display plane - unlike deploy.sh, whose payloads are headless."
fi
