#!/usr/bin/env bash
# =============================================================================
# scripts/build-ps5-opengl.sh - build (or unpack) the ps5-opengl-core33 SDK.
#
#   ./scripts/build-ps5-opengl.sh                 build from the submodule source
#   ./scripts/build-ps5-opengl.sh --frozen-archive <tar>   unpack + verify a
#                                                prebuilt SDK bundle instead
#   ./scripts/build-ps5-opengl.sh --check         toolchain preflight only
#
# Output (both modes):
#   third_party/ps5-opengl/build/sdk/ps5-opengl-core33/
#     -> headers, static archives, share/.../ps5-opengl-core33.mk, manifest.sha256
#   scripts/package-app.sh --gl-smoke consumes that .mk to link the #77 GL smoke
#   into the .ffpfsc app module.
#
# This is GL-1 (#77) scaffolding, part of the render overhaul
# (docs/evo-pro/opengl-render-overhaul.md + docs/evo-pro/gl1-spike.md).
#
# The from-source path needs a heavier toolchain than the pinned dev image
# carries (Clang/LLD 21.1.8 for the x86_64-sie-ps5 SDK wrappers, Meson 1.10.1,
# glslang + SPIR-V Tools, Python Mako/PyYAML). Run --check first. The opt-in
# overlay that supplies them is Dockerfile.ps5-opengl / docker-compose.ps5-opengl.yml
# - it is deliberately NOT wired into the default image.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

SUBMOD="${REPO_ROOT}/third_party/ps5-opengl"
PATCH="${REPO_ROOT}/patches/ps5-opengl/0001-recoverable-fail.patch"
SDK_OUT="${SUBMOD}/build/sdk/ps5-opengl-core33"

MODE="source"
FROZEN=""
CHECK_ONLY=0

while (( $# )); do
    case "$1" in
        --frozen-archive) shift; MODE="frozen"; FROZEN="${1:?--frozen-archive needs a path to the SDK tar}" ;;
        --check)          CHECK_ONLY=1 ;;
        -h|--help)        sed -n '2,26p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

if ! in_container; then
    FWD=()
    (( CHECK_ONLY ))      && FWD+=(--check)
    [[ -n "${FROZEN}" ]]  && FWD+=(--frozen-archive "${FROZEN}")
    reexec_in_container "build-ps5-opengl.sh" "${FWD[@]+"${FWD[@]}"}"
fi

# ---------------------------------------------------------------------------
# 1. Submodule present + pinned.
# ---------------------------------------------------------------------------
begin "checking the ps5-opengl submodule"
# gitlink SHA from the index (covers the not-yet-committed case), else HEAD.
PINNED="$(git -C "${REPO_ROOT}" ls-files -s third_party/ps5-opengl 2>/dev/null | awk '{print $2}')"
[[ -n "${PINNED}" ]] || PINNED="$(git -C "${REPO_ROOT}" ls-tree HEAD third_party/ps5-opengl 2>/dev/null | awk '{print $3}')"
[[ -n "${PINNED}" ]] || die "third_party/ps5-opengl is not a tracked submodule (index + HEAD both empty)"
if [[ ! -f "${SUBMOD}/Makefile" || ! -d "${SUBMOD}/src/gallium/ps5" ]]; then
    log "submodule not checked out - git submodule update --init (checkout to the pinned SHA)"
    git -C "${REPO_ROOT}" -c "submodule.third_party/ps5-opengl.update=checkout" \
        submodule update --init third_party/ps5-opengl
fi
need_file "${SUBMOD}/src/gallium/ps5/ps5_screen.c" "ps5-opengl submodule is empty"
HAVE="$(git -C "${SUBMOD}" rev-parse HEAD)"
if [[ "${PINNED}" != "${HAVE}" ]]; then
    die "ps5-opengl is at ${HAVE:0:12} but the gitlink pins ${PINNED:0:12}.
       The submodule drifted (a stray fetch/pull). Restore the pin:
         git -C '${SUBMOD}' checkout ${PINNED}
       (or, if you intend to bump: also refresh the _Exit patch + gl1-spike.md)"
fi
ok "ps5-opengl @ ${HAVE:0:12} (matches gitlink)"

# ---------------------------------------------------------------------------
# 2. Verify the _Exit -> ps5gl_fatal patch still applies (it is applied to the
#    submodule working tree in section 4, only in a real build).
#    A submodule bump must re-verify this (see gl1-spike.md).
# ---------------------------------------------------------------------------
begin "checking patches/ps5-opengl/0001-recoverable-fail.patch"
need_file "${PATCH}"
if git -C "${SUBMOD}" apply --reverse --check "${PATCH}" >/dev/null 2>&1; then
    ok "_Exit patch: already applied to the working tree"
    PATCH_STATE="applied"
elif git -C "${SUBMOD}" apply --check "${PATCH}" >/dev/null 2>&1; then
    ok "_Exit patch: applies cleanly"
    PATCH_STATE="clean"
else
    die "0001-recoverable-fail.patch no longer applies to ${HAVE:0:12}.
       The submodule pin moved without the patch being refreshed. Regenerate it:
         cd third_party/ps5-opengl && <re-apply the 4 edits> && git diff > ../../patches/ps5-opengl/0001-recoverable-fail.patch
       See docs/evo-pro/gl1-spike.md."
fi

# ---------------------------------------------------------------------------
# 3. Toolchain preflight.
# ---------------------------------------------------------------------------
begin "toolchain preflight"
missing=0
check() {  # check <label> <test-cmd...>
    local label="$1"; shift
    if "$@" >/dev/null 2>&1; then
        ok "${label}"
    else
        warn "MISSING: ${label}"
        missing=$((missing + 1))
    fi
}
sdk_wrapper_clang="${PS5_PAYLOAD_SDK}/bin/prospero-clang"
check "PS5 Payload SDK v0.42 (dependencies.json pin)" test -x "${sdk_wrapper_clang}"
check "SDK wrapper clang is 21.x (Mesa/PSBC build path)" \
    bash -c "'${sdk_wrapper_clang}' --version 2>/dev/null | grep -qE 'clang version 21\\.'"
check "clang-18 / ld.lld-18 (native-app link glue)" bash -c 'command -v clang-18 && command -v ld.lld-18'
check "meson >= 1.10.1" bash -c '
    v=$(meson --version 2>/dev/null) || exit 1
    [ "$(printf "1.10.1\n%s\n" "$v" | sort -V | head -n1)" = "1.10.1" ]'
check "ninja" command -v ninja
check "python3 >= 3.12" bash -c 'python3 -c "import sys; sys.exit(0 if sys.version_info>=(3,12) else 1)"'
check "python: mako" python3 -c 'import mako'
check "python: yaml" python3 -c 'import yaml'
check "python: packaging" python3 -c 'import packaging'
check "glslangValidator" command -v glslangValidator
check "spirv-tools (spirv-as)" command -v spirv-as
check "bison + flex" bash -c 'command -v bison && command -v flex'

if (( missing )); then
    echo ""
    warn "${missing} prerequisite(s) missing for the from-source SDK build."
    cat >&2 <<'EOF'

   The pinned dev image (LLVM 18, meson from apt) does not carry the Mesa/PSBC
   build toolchain. Two ways forward:

   A) Opt into the overlay image (adds Clang 21.1.8, meson 1.10.1, glslang,
      spirv-tools, python-mako/pyyaml on top of the base):

        docker compose -f docker-compose.yml -f docker-compose.ps5-opengl.yml build ps5-dev
        docker compose -f docker-compose.yml -f docker-compose.ps5-opengl.yml \
          run --rm ps5-dev ./scripts/build-ps5-opengl.sh

   B) Consume a prebuilt SDK bundle (blackbearreloaded release
      v0.1.0-perf20260907-sampled), FW-6.02 sample-validated:

        ./scripts/build-ps5-opengl.sh --frozen-archive path/to/ps5-opengl-sdk.tar.gz

   See docs/evo-pro/gl1-spike.md for the toolchain gap analysis.
EOF
    (( CHECK_ONLY )) && exit 1
    [[ "${MODE}" == "frozen" ]] || die "toolchain incomplete - see above"
fi
(( CHECK_ONLY )) && { ok "preflight passed"; exit 0; }

# ---------------------------------------------------------------------------
# 4. Apply the _Exit patch to the submodule working tree (real build only).
# ---------------------------------------------------------------------------
if [[ "${MODE}" == "source" && "${PATCH_STATE}" == "clean" ]]; then
    begin "applying 0001-recoverable-fail.patch to the submodule working tree"
    git -C "${SUBMOD}" apply "${PATCH}"
    ok "patched (4 _Exit sites -> ps5gl_fatal); submodule now shows dirty - expected"
fi

# ---------------------------------------------------------------------------
# 4a. Frozen-archive mode: unpack + verify, skip the build.
# ---------------------------------------------------------------------------
if [[ "${MODE}" == "frozen" ]]; then
    need_file "${FROZEN}" "frozen SDK archive not found"
    begin "unpacking frozen SDK bundle"
    work="$(mktemp -d)"
    tar -xf "${FROZEN}" -C "${work}"
    # The bundle roots at <name>/sdk/ ; find the ps5-opengl-core33 dir.
    src_sdk="$(find "${work}" -type d -name 'ps5-opengl-core33' -print -quit)"
    [[ -n "${src_sdk}" ]] || src_sdk="$(find "${work}" -type f -name 'manifest.sha256' -printf '%h\n' -quit)"
    [[ -n "${src_sdk}" ]] || die "archive has no ps5-opengl-core33/ or manifest.sha256"
    mkdir -p "$(dirname "${SDK_OUT}")"
    rm -rf "${SDK_OUT}"
    cp -a "${src_sdk}" "${SDK_OUT}"
    rm -rf "${work}"
    begin "verifying manifest"
    ( cd "${SDK_OUT}" && sha256sum --check --strict manifest.sha256 >/dev/null ) \
        || die "manifest.sha256 mismatch - archive is corrupt or repacked"
    ok "frozen SDK staged at ${SDK_OUT#"${REPO_ROOT}/"}"
    exit 0
fi

# ---------------------------------------------------------------------------
# 4b. From-source build.
# ---------------------------------------------------------------------------
load_sdk
export PS5_PAYLOAD_SDK
# dependencies.json pins payload_sdk v0.42 == the image pin, so let the
# ps5-opengl Makefile use our SDK directly rather than fetching its own.
export PS5_NATIVE_APP_TEMPLATE="${PS5_NATIVE_APP_TEMPLATE:-}"

BUILD_LOG="${LOG_OUT}/ps5-opengl-$(date -u +%Y%m%dT%H%M%SZ).log"
mkdirs

begin "make source-fetch (immutable-commit + Mesa archive hash check)"
if ! make -C "${SUBMOD}" source-fetch >"${BUILD_LOG}" 2>&1; then
    tail -40 "${BUILD_LOG}" | sed 's/^/  /'
    die "source-fetch failed. Full log: ${BUILD_LOG#"${REPO_ROOT}/"}"
fi
ok "sources fetched + hash-checked"

begin "make sdk (host + PS5 compiler libs, Mesa, native backend) - slow"
if ! make -C "${SUBMOD}" sdk >>"${BUILD_LOG}" 2>&1; then
    tail -60 "${BUILD_LOG}" | sed 's/^/  /'
    die "make sdk failed. Full log: ${BUILD_LOG#"${REPO_ROOT}/"}"
fi
need_file "${SDK_OUT}/share/ps5-opengl-core33/ps5-opengl-core33.mk" \
    "make sdk did not install the consumer .mk"
ok "SDK built -> ${SDK_OUT#"${REPO_ROOT}/"}"

begin "verify-installed-sdk.sh (344 Core exports, Make/pkg-config/CMake link)"
if ! bash "${SUBMOD}/tools/verify-installed-sdk.sh" >>"${BUILD_LOG}" 2>&1; then
    tail -40 "${BUILD_LOG}" | sed 's/^/  /'
    die "SDK interface verification failed. Full log: ${BUILD_LOG#"${REPO_ROOT}/"}"
fi
ok "SDK interface verified"

echo ""
ok "ps5-opengl-core33 ready. Next:"
echo "   ./scripts/package-app.sh --gl-smoke --usb-remote --ffpfsc"
echo "   ./scripts/deploy-app.sh --ffpfsc      # then touch /mnt/usb0/evo_gl_smoke, launch"
