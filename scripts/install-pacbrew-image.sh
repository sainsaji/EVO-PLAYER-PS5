#!/usr/bin/env bash
# =============================================================================
# install-pacbrew-image.sh - install prebuilt PS5 libraries during docker build.
#
# WHAT THIS PROVIDES
#   ps5-payload-dev/pacbrew-repo is a set of Arch-style PKGBUILDs that
#   cross-compile ~60 libraries against the PS5 Payload SDK. Its CI
#   (.github/workflows/ps5-payload-libs.yml) runs ci-libs.sh and then
#       tar -czf ps5-payload-dev.tar.gz -C / opt/ps5-payload-sdk
#   so each release ships a complete prebuilt sysroot.
#
# WHY WE NEED IT
#   ProsperoPlayer's Makefile links absolute paths into that sysroot:
#       $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libSDL2.a
#       .../libavformat.a .../libavcodec.a .../libswresample.a
#       .../libavutil.a   .../libswscale.a
#   The FFmpeg in this tarball is 7.0.1 (pacbrew-repo/ffmpeg/PKGBUILD:
#   pkgver=7.0.1). That is the baseline EVO Player must first reproduce
#   before any custom FFmpeg is introduced.
#
# WHY A SURGICAL COPY INSTEAD OF `tar -x -C /`
#   The tarball also contains pacbrew's own (possibly older) copy of the SDK.
#   Extracting it wholesale over /opt/ps5-payload-sdk would silently downgrade
#   the SDK we pinned in the previous build stage. So we extract to a temp dir
#   and copy ONLY target/user/homebrew (the ports prefix, PS5_HBROOT), leaving
#   the pinned SDK's bin/, toolchain/, include/ and target/lib/ untouched.
#
# Inputs (Docker ARGs):
#   PACBREW_VERSION   release tag, e.g. v0.39
#   INSTALL_PACBREW   1 = install (default), 0 = skip
#   PS5_PAYLOAD_SDK   /opt/ps5-payload-sdk
# =============================================================================
set -euo pipefail

PACBREW_VERSION="${PACBREW_VERSION:-v0.39}"
INSTALL_PACBREW="${INSTALL_PACBREW:-1}"
PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

REPO="https://github.com/ps5-payload-dev/pacbrew-repo"

die() { echo "ERROR [install-pacbrew]: $*" >&2; exit 1; }
log() { echo "==> [pacbrew] $*"; }

if [[ "${INSTALL_PACBREW}" != "1" ]]; then
    log "INSTALL_PACBREW=0 - skipping prebuilt library sysroot."
    log "NOTE: ProsperoPlayer/EVO Player will NOT link without SDL2+FFmpeg."
    exit 0
fi

[[ -d "${PS5_PAYLOAD_SDK}" ]] || die "SDK not installed yet at ${PS5_PAYLOAD_SDK}"

tmp="$(mktemp -d)"
trap 'rm -rf -- "${tmp}"' EXIT

url="${REPO}/releases/download/${PACBREW_VERSION}/ps5-payload-dev.tar.gz"
log "downloading ${url}"
wget -q -O "${tmp}/pacbrew.tar.gz" "${url}" || die "download failed: ${url}"

log "extracting"
mkdir -p "${tmp}/x"
tar -xzf "${tmp}/pacbrew.tar.gz" -C "${tmp}/x" || die "extract failed"

src="${tmp}/x/opt/ps5-payload-sdk"
[[ -d "${src}" ]] || die "unexpected tarball layout: no opt/ps5-payload-sdk"

hb="${src}/target/user/homebrew"
[[ -d "${hb}" ]] || die "tarball has no target/user/homebrew - cannot supply FFmpeg/SDL2"

log "installing ports prefix -> ${PS5_PAYLOAD_SDK}/target/user/homebrew"
mkdir -p "${PS5_PAYLOAD_SDK}/target/user/homebrew"
cp -a "${hb}/." "${PS5_PAYLOAD_SDK}/target/user/homebrew/"

# pacbrew also ships a few extra host-side helpers (prospero-shsrv-shell,
# prospero-websrv-elfldr). Add only ones the pinned SDK does not already have.
if [[ -d "${src}/bin" ]]; then
    for f in "${src}/bin"/*; do
        [[ -f "${f}" ]] || continue
        target="${PS5_PAYLOAD_SDK}/bin/$(basename "${f}")"
        if [[ ! -e "${target}" ]]; then
            cp -a "${f}" "${target}"
            log "added host helper $(basename "${f}")"
        fi
    done
fi

# -----------------------------------------------------------------------------
# Verify the libraries ProsperoPlayer actually links are present, and record
# the FFmpeg version so the build logs are self-describing.
# -----------------------------------------------------------------------------
libdir="${PS5_PAYLOAD_SDK}/target/user/homebrew/lib"
missing=()
for l in libSDL2.a libavformat.a libavcodec.a libswresample.a libavutil.a libswscale.a; do
    [[ -f "${libdir}/${l}" ]] || missing+=("${l}")
done
if (( ${#missing[@]} )); then
    die "pacbrew ${PACBREW_VERSION} did not provide: ${missing[*]}"
fi

ffver="unknown"
vhdr="${PS5_PAYLOAD_SDK}/target/user/homebrew/include/libavutil/ffversion.h"
if [[ -f "${vhdr}" ]]; then
    ffver="$(sed -n 's/.*FFMPEG_VERSION *"\([^"]*\)".*/\1/p' "${vhdr}")"
fi

cat > "${PS5_PAYLOAD_SDK}/EVO_PACBREW_VERSION" <<EOF
pacbrew_repo_version=${PACBREW_VERSION}
ffmpeg_version=${ffver}
ffmpeg_pkgbuild_pkgver=7.0.1
installed_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

log "ok - FFmpeg ${ffver}, SDL2 and friends installed"
cat "${PS5_PAYLOAD_SDK}/EVO_PACBREW_VERSION"
