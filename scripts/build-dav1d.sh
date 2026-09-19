#!/usr/bin/env bash
#
# build-dav1d.sh — cross-build libdav1d for the PS5 and install it into the
# SDK sysroot, so FFmpeg can be configured with --enable-libdav1d.
#
# WHY THIS EXISTS
#   AV1 playback segfaulted the app (see output/logs/sweep.md, defect 4).
#   build-ffmpeg.sh passes --enable-decoder=av1 with the comment "native
#   decoder; slow, but present" - but FFmpeg's `av1` decoder (av1dec.c) is a
#   HWACCEL-ONLY wrapper with no software decode path. The PS5 has no AV1
#   hardware decoder, so at the first frame it logs
#
#       Your platform doesn't suppport hardware accelerated AV1 decoding.
#
#   (typo upstream's) and then dereferences null inside libavcodec. The
#   decoder-presence check at build-ffmpeg.sh:423 passed the whole time
#   because a decoder of that NAME exists.
#
#   dav1d is the real software AV1 decoder. It is heavily hand-optimised and
#   threaded; the console's Zen 2 8-core handles 1080p comfortably.
#
# WHY GITHUB AND NOT VIDEOLAN
#   downloads.videolan.org and code.videolan.org are both unreachable from the
#   build container; github.com is not. github.com/videolan/dav1d is the
#   upstream project's own mirror.
#
# USAGE
#   ./scripts/build-dav1d.sh                 build only, into a staging dir
#   ./scripts/build-dav1d.sh --install       ... and install into the sysroot
#   ./scripts/build-dav1d.sh --version 1.5.1
#
#   After --install, rebuild FFmpeg so it picks the library up:
#       ./scripts/build-ffmpeg.sh --install
#
set -euo pipefail
SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPTS_DIR}/common.sh"

DAV1D_VERSION="1.5.1"
DO_INSTALL=0
JOBS="$(nproc)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install) DO_INSTALL=1 ;;
        --version) shift; DAV1D_VERSION="${1:?--version needs a value}" ;;
        --jobs)    shift; JOBS="${1:?--jobs needs a value}" ;;
        -h|--help) sed -n '2,32p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
    shift
done

need_cmd wget tar ninja nasm
load_sdk

SRC_ROOT="${OUTPUT_DIR}/third_party"
SRC_DIR="${SRC_ROOT}/dav1d-${DAV1D_VERSION}"
BUILD_DIR="${SRC_DIR}/build-ps5"
STAGE_DIR="${OUTPUT_DIR}/third_party/dav1d-stage"
TARBALL="${SRC_ROOT}/dav1d-${DAV1D_VERSION}.tar.gz"

mkdir -p "${SRC_ROOT}"

log "dav1d ${DAV1D_VERSION}"
echo "  source : ${SRC_DIR}"
echo "  stage  : ${STAGE_DIR}${PREFIX}"

begin "fetching source"
if [[ ! -f "${TARBALL}" ]]; then
    wget -q --show-progress -O "${TARBALL}.part" \
        "https://github.com/videolan/dav1d/archive/refs/tags/${DAV1D_VERSION}.tar.gz" \
      || die "download failed for dav1d ${DAV1D_VERSION}"
    mv "${TARBALL}.part" "${TARBALL}"
else
    ok "tarball already present"
fi

if [[ ! -d "${SRC_DIR}" ]]; then
    begin "extracting"
    tar -xf "${TARBALL}" -C "${SRC_ROOT}" || die "extract failed"
fi
need_file "${SRC_DIR}/meson.build" "dav1d source tree looks wrong."

# -----------------------------------------------------------------------------
# prospero-meson injects --cross-file=<sdk>/toolchain/prospero.ini, which
# already pins system=freebsd, cpu=x86_64, default_library=static and
# prefix=/user/homebrew. Only dav1d's own options are set here.
#
# Tools/tests/examples are off because the cross file sets
# needs_exe_wrapper=true - meson cannot run a PS5 binary on the host, and
# building them would only produce artifacts nothing installs.
# -----------------------------------------------------------------------------
begin "configure (meson)"
rm -rf "${BUILD_DIR}"
MESON_LOG="${LOG_OUT}/dav1d-${DAV1D_VERSION}-setup.log"
mkdir -p "${LOG_OUT}"
if ! "${MESON}" setup "${BUILD_DIR}" "${SRC_DIR}" \
        --buildtype=release \
        --default-library=static \
        -Denable_tools=false \
        -Denable_tests=false \
        > "${MESON_LOG}" 2>&1; then
    echo ""
    echo "--- last 40 lines of meson setup ---"
    tail -40 "${MESON_LOG}" | sed 's/^/  /'
    die "meson setup failed. Full log: ${MESON_LOG#"${REPO_ROOT}/"}"
fi
ok "configured"

begin "compiling (-j${JOBS})"
BUILD_LOG="${LOG_OUT}/dav1d-${DAV1D_VERSION}-build.log"
if ! ninja -C "${BUILD_DIR}" -j"${JOBS}" > "${BUILD_LOG}" 2>&1; then
    echo ""
    echo "--- last 40 lines of build ---"
    tail -40 "${BUILD_LOG}" | sed 's/^/  /'
    die "dav1d build failed. Full log: ${BUILD_LOG#"${REPO_ROOT}/"}"
fi
ok "compiled"

begin "staging install"
rm -rf "${STAGE_DIR}"
DESTDIR="${STAGE_DIR}" ninja -C "${BUILD_DIR}" install >/dev/null 2>&1 \
    || die "dav1d staged install failed"

STAGED_LIB="${STAGE_DIR}${PREFIX}/lib"
need_file "${STAGED_LIB}/libdav1d.a" "dav1d did not produce a static library."
# A host-x86_64-Linux .a here would link but never run on the console; the
# cross file is the only thing preventing that, so check rather than trust.
if ! "${PS5_PAYLOAD_SDK}/bin/prospero-nm" "${STAGED_LIB}/libdav1d.a" >/dev/null 2>&1; then
    die "libdav1d.a is not readable by the cross nm - wrong toolchain?"
fi
ok "staged $(du -h "${STAGED_LIB}/libdav1d.a" | cut -f1) libdav1d.a"

if (( DO_INSTALL )); then
    begin "installing into the SDK sysroot"
    cp -a "${STAGE_DIR}${PREFIX}/." "${PS5_SYSROOT}${PREFIX}/"
    if [[ -x "${PS5_CROSS_FIX_ROOT:-}" ]]; then
        "${PS5_CROSS_FIX_ROOT}" "${PS5_SYSROOT}${PREFIX}"
    fi
    ok "installed to ${PS5_SYSROOT}${PREFIX}"
    echo ""
    echo "   next: ./scripts/build-ffmpeg.sh --install   (to link dav1d in)"
else
    echo ""
    echo "   staged only. install into the sysroot with:"
    echo "     ./scripts/build-dav1d.sh --install"
fi
