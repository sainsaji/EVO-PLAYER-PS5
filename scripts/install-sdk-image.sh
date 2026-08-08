#!/usr/bin/env bash
# =============================================================================
# install-sdk-image.sh - install the PS5 Payload SDK during `docker build`.
#
# Not intended to be run by hand; use scripts/setup-sdk.sh inside a running
# container to refresh or rebuild the SDK. The Dockerfile COPYs and runs this.
#
# Inputs (Docker ARGs, visible here as environment variables):
#   PS5_SDK_VERSION        git tag / release tag, e.g. v0.42
#   BUILD_SDK_FROM_SOURCE  0 = use the released zip (default), 1 = build
#   PS5_PAYLOAD_SDK        install prefix, /opt/ps5-payload-sdk
# =============================================================================
set -euo pipefail

PS5_SDK_VERSION="${PS5_SDK_VERSION:-v0.42}"
BUILD_SDK_FROM_SOURCE="${BUILD_SDK_FROM_SOURCE:-0}"
PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

SDK_REPO="https://github.com/ps5-payload-dev/sdk"

die() { echo "ERROR [install-sdk-image]: $*" >&2; exit 1; }
log() { echo "==> [sdk] $*"; }

# -----------------------------------------------------------------------------
# Sanity: the SDK's Makefile.inc locates the toolchain through llvm-config.
# Fail loudly here rather than emitting a confusing error deep in a sub-make.
# -----------------------------------------------------------------------------
command -v llvm-config >/dev/null 2>&1 \
  || die "llvm-config not on PATH - the LLVM_VERSION build-arg is probably wrong."
log "host llvm-config: $(llvm-config --version) at $(llvm-config --bindir)"
command -v clang >/dev/null 2>&1 || die "clang not on PATH."
command -v ld.lld >/dev/null 2>&1 || die "ld.lld not on PATH (lld package missing)."

if [[ "${BUILD_SDK_FROM_SOURCE}" == "1" ]]; then
    # -------------------------------------------------------------------------
    # Source build. Mirrors ps5-payload-dev/sdk ci.sh:
    #   make install  ->  libcxx.sh
    # libcxx.sh downloads llvm-project 18.1.8 sources and cross-builds
    # libc++/libc++abi/libunwind into $PS5_SYSROOT. That step is what makes
    # `-lc++ -lc++abi` (used by ProsperoPlayer) link.
    # -------------------------------------------------------------------------
    log "building SDK ${PS5_SDK_VERSION} from source"
    tmp="$(mktemp -d)"
    git clone --depth 1 --branch "${PS5_SDK_VERSION}" "${SDK_REPO}" "${tmp}/sdk" \
      || die "failed to clone ${SDK_REPO} at ${PS5_SDK_VERSION}"

    make -C "${tmp}/sdk" DESTDIR="${PS5_PAYLOAD_SDK}" install \
      || die "SDK 'make install' failed"

    log "bootstrapping libc++ (llvm-project 18.1.8 runtimes)"
    PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK}" "${tmp}/sdk/libcxx.sh" \
      || die "libcxx.sh failed - C++ payloads would not link"

    rm -rf "${tmp}"
else
    # -------------------------------------------------------------------------
    # Released zip. Upstream CI already ran `make install` AND `libcxx.sh`
    # before zipping, so this artifact contains the prebuilt libc++.
    # -------------------------------------------------------------------------
    log "installing SDK release ${PS5_SDK_VERSION} (prebuilt zip)"
    tmp="$(mktemp -d)"
    url="${SDK_REPO}/releases/download/${PS5_SDK_VERSION}/ps5-payload-sdk.zip"
    wget -q -O "${tmp}/sdk.zip" "${url}" \
      || die "download failed: ${url}"

    # The zip contains a top-level ps5-payload-sdk/ directory.
    unzip -q "${tmp}/sdk.zip" -d "${tmp}/x" || die "unzip failed"
    mkdir -p "${PS5_PAYLOAD_SDK}"
    if [[ -d "${tmp}/x/ps5-payload-sdk" ]]; then
        cp -a "${tmp}/x/ps5-payload-sdk/." "${PS5_PAYLOAD_SDK}/"
    else
        cp -a "${tmp}/x/." "${PS5_PAYLOAD_SDK}/"
    fi
    rm -rf "${tmp}"
fi

# -----------------------------------------------------------------------------
# Record exactly what was installed. docs/ and the README reference this file,
# and scripts/build.sh prints it so build logs are self-describing.
# -----------------------------------------------------------------------------
cat > "${PS5_PAYLOAD_SDK}/EVO_SDK_VERSION" <<EOF
ps5_payload_sdk_version=${PS5_SDK_VERSION}
installed_from=$([[ "${BUILD_SDK_FROM_SOURCE}" == "1" ]] && echo source || echo release-zip)
host_llvm_version=$(llvm-config --version)
host_clang=$(clang --version | head -1)
installed_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

# -----------------------------------------------------------------------------
# Verify the install actually produced a usable toolchain.
# -----------------------------------------------------------------------------
for f in toolchain/prospero.sh toolchain/prospero.mk bin/prospero-clang \
         bin/prospero-lld bin/prospero-deploy; do
    [[ -e "${PS5_PAYLOAD_SDK}/${f}" ]] || die "SDK install incomplete: missing ${f}"
done
chmod -R a+rX "${PS5_PAYLOAD_SDK}"
find "${PS5_PAYLOAD_SDK}/bin" -type f -exec chmod a+rx {} +

log "installed to ${PS5_PAYLOAD_SDK}"
cat "${PS5_PAYLOAD_SDK}/EVO_SDK_VERSION"
