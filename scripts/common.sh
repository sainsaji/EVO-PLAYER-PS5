#!/usr/bin/env bash
# =============================================================================
# scripts/common.sh - shared helpers. Sourced by every other script.
#
# Provides:
#   strict mode + an ERR trap that names the failing file/line/command
#   logging helpers (log/warn/die/step)
#   in_container / require_container / reexec_in_container
#   need_cmd / need_file / need_sdk
#   PS5 host validation
# =============================================================================

# -- Strict mode --------------------------------------------------------------
#   -e  stop at the first failing command
#   -u  a typo'd variable is an error, not an empty string
#   -o pipefail  a failure anywhere in a pipeline fails the pipeline
set -euo pipefail

# Colours only when attached to a terminal.
if [[ -t 1 ]] && [[ "${TERM:-dumb}" != "dumb" ]]; then
    _C_RED=$'\033[31m'; _C_YEL=$'\033[33m'; _C_GRN=$'\033[32m'
    _C_BLU=$'\033[36m'; _C_BLD=$'\033[1m';  _C_OFF=$'\033[0m'
else
    _C_RED=''; _C_YEL=''; _C_GRN=''; _C_BLU=''; _C_BLD=''; _C_OFF=''
fi

_SCRIPT_NAME="$(basename "${BASH_SOURCE[1]:-${BASH_SOURCE[0]}}")"

log()  { echo "${_C_BLU}==>${_C_OFF} ${_C_BLD}$*${_C_OFF}"; }
step() { echo "${_C_BLU}--- $*${_C_OFF}"; }
ok()   { echo "${_C_GRN}  ok${_C_OFF} $*"; }
warn() { echo "${_C_YEL}warning:${_C_OFF} $*" >&2; }
die()  { echo "${_C_RED}ERROR${_C_OFF} [${_SCRIPT_NAME}]: $*" >&2; exit 1; }

# -----------------------------------------------------------------------------
# ERR trap: say exactly which step failed, which the task brief asks for.
# -----------------------------------------------------------------------------
_on_err() {
    local exit_code=$?
    local line=${1:-?}
    local cmd=${2:-?}
    echo "" >&2
    echo "${_C_RED}${_C_BLD}BUILD FAILED${_C_OFF}" >&2
    echo "  script : ${_SCRIPT_NAME}" >&2
    echo "  line   : ${line}" >&2
    echo "  command: ${cmd}" >&2
    echo "  exit   : ${exit_code}" >&2
    if [[ -n "${_CURRENT_STEP:-}" ]]; then
        echo "  step   : ${_CURRENT_STEP}" >&2
    fi
    exit "${exit_code}"
}
trap '_on_err "${LINENO}" "${BASH_COMMAND}"' ERR

# Set a human-readable label for the current phase (reported by the ERR trap).
_CURRENT_STEP=""
begin() { _CURRENT_STEP="$*"; step "$*"; }

# -- Paths --------------------------------------------------------------------
SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPTS_DIR}/.." && pwd)"
export REPO_ROOT SCRIPTS_DIR

# Load .env / .env.local if present
if [[ -f "${REPO_ROOT}/.env" ]]; then
    set +u; set -a
    # shellcheck disable=SC1091
    source "${REPO_ROOT}/.env"
    set +a; set -u
fi
if [[ -f "${REPO_ROOT}/.env.local" ]]; then
    set +u; set -a
    # shellcheck disable=SC1091
    source "${REPO_ROOT}/.env.local"
    set +a; set -u
fi

: "${PS5_PAYLOAD_SDK:=/opt/ps5-payload-sdk}"
: "${PS5_PORT:=9021}"   # ps5-payload-elfldr default (host/bin/prospero-deploy)
: "${PS5_MAC:=}"
export PS5_PAYLOAD_SDK PS5_PORT PS5_MAC

OUTPUT_DIR="${REPO_ROOT}/output"
ELF_OUT="${OUTPUT_DIR}/elf"
PKG_OUT="${OUTPUT_DIR}/pkg"
LOG_OUT="${OUTPUT_DIR}/logs"
export OUTPUT_DIR ELF_OUT PKG_OUT LOG_OUT

# -- Requirement checks -------------------------------------------------------
need_cmd() {
    local c
    for c in "$@"; do
        command -v "${c}" >/dev/null 2>&1 \
          || die "required command '${c}' not found on PATH.
       Inside the container this means the image is out of date - rebuild with:
           docker compose build --no-cache"
    done
}

need_file() {
    [[ -e "$1" ]] || die "required file missing: $1${2:+
       $2}"
}

# -----------------------------------------------------------------------------
# Are we inside the dev container? The image sets PS5_PAYLOAD_SDK and ships
# the SDK, so test for the toolchain rather than for /.dockerenv (which is
# absent under some runtimes).
# -----------------------------------------------------------------------------
in_container() {
    [[ -f "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh" ]]
}

# Re-run the current script inside the container, preserving arguments.
# Lets the same script work from Windows PowerShell and from a container shell.
reexec_in_container() {
    local rel="${1}"; shift
    command -v docker >/dev/null 2>&1 \
      || die "not inside the dev container, and 'docker' is not available on this host.
       Install Docker Desktop, or run this script from inside the container."

    log "not in container - re-running via docker compose"
    exec docker compose -f "${REPO_ROOT}/docker-compose.yml" run --rm \
        -e "PS5_HOST=${PS5_HOST:-}" \
        -e "PS5_MAC=${PS5_MAC:-}" \
        -e "PS5_PORT=${PS5_PORT}" \
        -e "EXTRA_CFLAGS=${EXTRA_CFLAGS:-}" \
        ps5-dev "./scripts/${rel}" "$@"
}

require_container() {
    in_container || die "this script must run inside the dev container.
       Start one with:  ./scripts/shell.sh
       or:              docker compose run --rm ps5-dev bash"
}

# -----------------------------------------------------------------------------
# Load the SDK cross-toolchain (exports CC/CXX/LD/AR/CMAKE/MESON/PKG_CONFIG/
# PS5_DEPLOY/PS5_SYSROOT/PREFIX...). Safe to call more than once.
# -----------------------------------------------------------------------------
load_sdk() {
    need_file "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh" \
        "The PS5 Payload SDK is not installed. Run ./scripts/setup-sdk.sh"
    # prospero.sh references unset vars in places; relax -u just for the source.
    set +u
    # shellcheck disable=SC1091
    source "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"
    set -u
}

# Print the pinned versions recorded at image build time.
sdk_version_banner() {
    if [[ -f "${PS5_PAYLOAD_SDK}/EVO_SDK_VERSION" ]]; then
        sed 's/^/  /' "${PS5_PAYLOAD_SDK}/EVO_SDK_VERSION"
    fi
    if [[ -f "${PS5_PAYLOAD_SDK}/EVO_PACBREW_VERSION" ]]; then
        sed 's/^/  /' "${PS5_PAYLOAD_SDK}/EVO_PACBREW_VERSION"
    fi
}

# -----------------------------------------------------------------------------
# PS5 host/port validation, shared by deploy.sh and the test targets.
# -----------------------------------------------------------------------------
auto_detect_ps5_from_mac() {
    local target_mac="${1:-${PS5_MAC:-}}"
    [[ -n "${target_mac}" ]] || return 1
    target_mac="$(echo "${target_mac}" | tr '[:upper:]-' '[:lower:]:')"

    if [[ -f /proc/net/arp ]]; then
        local found_ip
        found_ip=$(awk -v mac="${target_mac}" 'tolower($4) == mac {print $1}' /proc/net/arp | head -n 1)
        if [[ -n "${found_ip}" ]]; then
            PS5_HOST="${found_ip}"
            export PS5_HOST
            return 0
        fi
    fi

    if command -v ip >/dev/null 2>&1; then
        local found_ip
        found_ip=$(ip neigh 2>/dev/null | awk -v mac="${target_mac}" 'tolower($5) == mac {print $1}' | head -n 1)
        if [[ -n "${found_ip}" ]]; then
            PS5_HOST="${found_ip}"
            export PS5_HOST
            return 0
        fi
    fi

    return 1
}

require_ps5_host() {
    if [[ -z "${PS5_HOST:-}" ]]; then
        if [[ -n "${PS5_MAC:-}" ]] && auto_detect_ps5_from_mac "${PS5_MAC}"; then
            ok "Auto-detected PS5 IP from MAC (${PS5_MAC}): ${PS5_HOST}"
            return 0
        fi

        die "PS5_HOST is not set and could not auto-detect from MAC.

       Point it at your console (never hard-coded in this repo):
           PS5_HOST=192.168.1.50 $0 $*
       or persist it for compose in a .env file at the repo root:
           PS5_HOST=192.168.1.50
           PS5_MAC=00:11:22:33:44:55   # optional, enables auto-detect by MAC

       Find the address on the console under:
           Settings -> Network -> Connection Status"
    fi
}

# Confirm the console's ELF loader is actually accepting connections.
check_ps5_reachable() {
    local host="${1}" port="${2}" timeout="${3:-5}"
    need_cmd nc
    if nc -z -w "${timeout}" "${host}" "${port}" 2>/dev/null; then
        ok "${host}:${port} is reachable"
        return 0
    fi
    return 1
}

mkdirs() { mkdir -p "${ELF_OUT}" "${PKG_OUT}" "${LOG_OUT}"; }

# -----------------------------------------------------------------------------
# Validate that a produced file really is a PS5 payload ELF and not, say, a
# host x86-64 Linux binary produced by an accidentally-unset CC.
# The SDK links freestanding x86-64 ELFs; the giveaway for a *wrong* build is
# an INTERP segment pointing at /lib64/ld-linux.
# -----------------------------------------------------------------------------
validate_elf() {
    local elf="$1"
    need_file "${elf}"
    need_cmd file

    local desc
    desc="$(file -b "${elf}")"
    case "${desc}" in
        *ELF\ 64-bit\ LSB*x86-64*) ;;
        *) die "${elf} is not a 64-bit x86-64 ELF (file says: ${desc})" ;;
    esac

    if command -v llvm-readelf >/dev/null 2>&1; then
        if llvm-readelf -l "${elf}" 2>/dev/null | grep -q '/lib64/ld-linux'; then
            die "${elf} links the host Linux dynamic loader.
       This means it was built with the host clang instead of the PS5
       cross toolchain. Source \$PS5_PAYLOAD_SDK/toolchain/prospero.sh, or
       use \$(PS5_PAYLOAD_SDK)/toolchain/prospero.mk from your Makefile."
        fi
    fi
    ok "$(basename "${elf}") - ${desc%%,*} ($(stat -c %s "${elf}") bytes)"
}
