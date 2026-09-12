#!/usr/bin/env bash
# =============================================================================
# scripts/package-app.sh - build EVO Player as a game-category app module.
#
#   ./scripts/package-app.sh                  build the full FFmpeg-software
#                                             player (Phase 1b task 4+)
#   ./scripts/package-app.sh --probe          build the sandbox probe instead
#   ./scripts/package-app.sh --rebuild-libc   force-regenerate the runtime shim
#   ./scripts/package-app.sh --ffpfsc         also emit a PFS image, like
#                                             ProsperoLight (needs MkPFS)
#   ./scripts/package-app.sh --usb-remote     + the scriptable FTP dev remote
#                                             (/mnt/usb0/evo_cmd + evo_status +
#                                             verbose vdec log) — off in release
#   ./scripts/package-app.sh --breadcrumbs    + boot-trace notification
#                                             popups (#51, off by default —
#                                             klog carries these otherwise)
#   ./scripts/package-app.sh --gl-smoke       + link ps5-opengl + the #77 GL-1
#                                             go/no-go probe. Needs
#                                             ./scripts/build-ps5-opengl.sh
#                                             first. Diagnostic build, like
#                                             --breadcrumbs. Runs on the console
#                                             only when /mnt/usb0/evo_gl_smoke
#                                             exists; see docs/evo-pro/gl1-spike.md
#   ./scripts/package-app.sh --gl             #79 GL-3 / #80 GL-4: the boot runs
#                                             on a persistent ps5-opengl GL/EGL
#                                             context. ALWAYS ON for the player
#                                             build - pass it only to be
#                                             explicit. Needs
#                                             ./scripts/build-ps5-opengl.sh
#                                             first. Mutually exclusive with
#                                             --gl-smoke. See docs/evo-pro/
#                                             opengl-render-overhaul.md
#                                             (--no-gl was retired by GL-4: the
#                                             CPU converters + tiled VideoOut
#                                             present it selected no longer
#                                             exist.)
#
# Compilation uses the native-app toolchain (tools/native-app/prospero-clang18:
# -femulated-tls -fno-plt -fno-stack-protector); the LINK + PS5-module
# conversion + FSELF signing + dist assembly come from tools/native-app/
# (vendored from ps5-native-app-boilerplate / ProsperoLight - see its README).
#
# Output: output/app/PPSA99039/{eboot.bin, sce_sys/param.json, sce_module/libc.prx}
# Deploy: ./scripts/deploy-app.sh   (FTP -> /data/homebrew/PPSA99039/)
#
# See docs/evo-pro/phase-1b-app-module.md.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

MODE="player"
REBUILD_LIBC=0
FFPFSC=0
USB_REMOTE=0
BREADCRUMBS=0
GL_SMOKE=0
GL_HDR_PROBE=0
GL_DEVICE=-1        # -1 = not specified; resolved below (default ON for the player build)
AGC_DEVICE=0
NATIVE_SECONDARY=0
NATIVE_SECONDARY_4K=0
NO_NATIVE_SECONDARY=0
NO_NATIVE_SECONDARY_4K=0
NATIVE_10BIT=0
while (( $# )); do
    case "$1" in
        --probe)        MODE="probe" ;;
        --player)       MODE="player" ;;
        --rebuild-libc) REBUILD_LIBC=1 ;;
        --ffpfsc)       FFPFSC=1 ;;
        --usb-remote)   USB_REMOTE=1 ;;   # dev: /mnt/usb0/evo_cmd + evo_status + verbose vdec log
        --breadcrumbs)  BREADCRUMBS=1 ;;  # #51: bring back the on-screen boot-trace popups
        --gl-smoke)     GL_SMOKE=1 ;;     # #77 GL-1: link ps5-opengl + the /mnt/usb0/evo_gl_smoke probe
        --gl-hdr-probe) GL_HDR_PROBE=1 ;; # Task 1: isolated HDR VideoOut probe
        --gl)           GL_DEVICE=1 ;;    # #79 GL-3 / #80 GL-4: persistent device GL context
        --agc)          AGC_DEVICE=1; GL_DEVICE=0 ;; # Bare-metal PS5 AGC: 100% GPU render interface
        --no-gl)        die "--no-gl was retired by GL-4 (#80): the CPU converters,
       tile_copy and the V8/V3/1080 backend dispatch it selected are deleted.
       GL or AGC are the only present paths." ;;
        --native-secondary)     NATIVE_SECONDARY=1 ;;                       # #41: HEVC + VP9 resident decoders are ON BY DEFAULT (4K, since 2026-09-11) — this flag is now a no-op kept for back-compat
        --native-secondary-4k)  NATIVE_SECONDARY=1; NATIVE_SECONDARY_4K=1 ;; # #41: HEVC/VP9 4K slots are ON BY DEFAULT since 2026-09-11 — this flag is now a no-op kept for back-compat
        --no-native-secondary)  NO_NATIVE_SECONDARY=1 ;;                    # #41: escape hatch — AVC-only, rollback to pre-2026-09-11 behaviour
        --no-native-secondary-4k) NO_NATIVE_SECONDARY_4K=1 ;;               # #41: escape hatch — HEVC/VP9 stay on but drop to 1080p, rollback to pre-2026-09-11 4K behaviour
        --native-10bit)         NATIVE_10BIT=1 ;;                           # #41 Phase D: HEVC Main10 + VP9 Profile 2 resident decoders — OFF by default, confirmed to break home-screen thumbnail decode alongside Phase B's three (2026-09-11)
        -h|--help)      sed -n '2,38p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done
# Present path resolution:
#   --agc       bare-metal AGC; links libSceAgc without ps5-opengl.
#   --gl        persistent device GL context via ps5-opengl (default when not --agc).
#   --gl-smoke  is its own boot cutover and mutually exclusive with --gl / --agc.
#   --probe     is the sandbox probe, not the player; it never presents.
if (( GL_DEVICE == -1 )); then
    if (( AGC_DEVICE || GL_SMOKE || GL_HDR_PROBE )) || [[ "${MODE}" != "player" ]]; then
        GL_DEVICE=0
    else
        GL_DEVICE=1
    fi
fi
(( AGC_DEVICE && GL_DEVICE )) && die "--agc and --gl are mutually exclusive"
(( AGC_DEVICE && GL_SMOKE )) && die "--agc and --gl-smoke are mutually exclusive"
(( AGC_DEVICE && GL_HDR_PROBE )) && die "--agc and --gl-hdr-probe are mutually exclusive"
(( AGC_DEVICE )) && [[ "${MODE}" != "player" ]] && die "--agc only applies to the player build (drop --agc with --${MODE})"
(( GL_SMOKE && GL_DEVICE )) && die "--gl and --gl-smoke are mutually exclusive (both cut the boot over to ps5-opengl)"
(( GL_HDR_PROBE && GL_DEVICE )) && die "--gl and --gl-hdr-probe are mutually exclusive (both cut the boot over to ps5-opengl)"
(( GL_SMOKE && GL_HDR_PROBE )) && die "--gl-smoke and --gl-hdr-probe are mutually exclusive"
(( GL_DEVICE )) && [[ "${MODE}" != "player" ]] && die "--gl only applies to the player build (drop --gl with --${MODE})"
# Flags that link ps5-opengl + share the SDK-consumption / undef-harvest / _Exit-wrap machinery.
GL_LINK=$(( GL_SMOKE || GL_DEVICE || GL_HDR_PROBE ))

if ! in_container; then
    FWD=(--"${MODE}")
    (( REBUILD_LIBC )) && FWD+=(--rebuild-libc)
    (( FFPFSC ))       && FWD+=(--ffpfsc)
    (( USB_REMOTE ))   && FWD+=(--usb-remote)
    (( BREADCRUMBS ))  && FWD+=(--breadcrumbs)
    (( GL_SMOKE ))     && FWD+=(--gl-smoke)
    (( GL_HDR_PROBE )) && FWD+=(--gl-hdr-probe)
    # Forward the RESOLVED choice, never the default, so the in-container build
    # cannot disagree with the host-side one.
    (( AGC_DEVICE ))   && FWD+=(--agc)
    (( GL_DEVICE ))    && FWD+=(--gl)
    (( NATIVE_SECONDARY_4K )) && FWD+=(--native-secondary-4k)
    (( NATIVE_SECONDARY && ! NATIVE_SECONDARY_4K )) && FWD+=(--native-secondary)
    (( NO_NATIVE_SECONDARY ))  && FWD+=(--no-native-secondary)
    (( NO_NATIVE_SECONDARY_4K )) && FWD+=(--no-native-secondary-4k)
    (( NATIVE_10BIT ))        && FWD+=(--native-10bit)
    reexec_in_container "package-app.sh" "${FWD[@]}"
fi

# --gl-smoke (#77, GL-1) / --gl (#79, GL-3): consume the ps5-opengl-core33 SDK
# that scripts/build-ps5-opengl.sh produced, compile the GL glue into the eboot,
# and link the Mesa/Gallium/PSBC archives. Since GL-4 (#80) the player build has
# no other present path, so a plain --ffpfsc needs the ps5-opengl SDK built.
# See docs/evo-pro/gl1-spike.md / gl2-render-interface.md.
GL_LINK_LIBS=()         # -L / -l args from the installed .mk, minus what EVO already links
GL_PUBLIC_CFLAGS=""
GL_FORCE_UNDEF=()
if (( GL_LINK )); then
    [[ "${MODE}" == "player" ]] || die "--gl / --gl-smoke only apply to the player build"
    GL_SDK="${REPO_ROOT}/third_party/ps5-opengl/build/sdk/ps5-opengl-core33"
    GL_MK="${GL_SDK}/share/ps5-opengl-core33/ps5-opengl-core33.mk"
    need_file "${GL_MK}" "ps5-opengl SDK not built. Run:  ./scripts/build-ps5-opengl.sh"
    [[ -d "${GL_SDK}/lib" ]] || die "ps5-opengl SDK has no lib/ — rebuild it"
    # Evaluate the installed .mk with make so $(PS5_OPENGL_PREFIX) etc. resolve
    # (parsing the file textually scrapes "GPL-3.0-or-later" as "-later").
    gl_mk_val() {
        printf 'gl_print:\n\t@printf "%%s" "$(%s)"\n' "$1" \
            | make --no-print-directory -f "${GL_MK}" -f - gl_print 2>/dev/null
    }
    GL_PUBLIC_CFLAGS="$(gl_mk_val PS5_OPENGL_PUBLIC_CFLAGS)"
    [[ -n "${GL_PUBLIC_CFLAGS}" ]] || GL_PUBLIC_CFLAGS="-DGL_GLEXT_PROTOTYPES=1 -I${GL_SDK}/include"
    GL_LINK_LIBS+=("-L${GL_SDK}/lib")
    # From PS5_OPENGL_LDLIBS keep -L/-l (order preserved), drop the system SCE
    # modules EVO already resolves (PRX stubs + target/lib/*.so), and lift
    # -Wl,-u,<sym> into GL_FORCE_UNDEF.
    for tok in $(gl_mk_val PS5_OPENGL_LDLIBS); do
        case "${tok}" in
            -lSceAgc|-lSceAgcDriver|-lSceVideoOut|-lkernel_web|-lSceSystemService) ;;
            -Wl,-u,*) GL_FORCE_UNDEF+=(-u "${tok#-Wl,-u,}") ;;
            -Wl,--*group) ;;                         # lld groups the archives itself
            -L*|-l*)  GL_LINK_LIBS+=("${tok}") ;;
        esac
    done
    # ps5-opengl's Gallium driver links a handful of sceAgc* / sceAgcDriver*
    # entry points directly (not via dlsym). Harvest the undefined sce* names
    # from the SDK archives so the PRX stubs (section 6b) cover them and the
    # dead-import guard recognises them as genuinely imported.
    # (llvm-nm -u under-reports on archives; list all and filter to `U` lines.)
    # (libPS5OpenGLCore33.a is a GNU ld GROUP script, not an object -> llvm-nm
    #  errors on it; `|| true` so `set -e` doesn't abort the harvest subshell.)
    GL_SCE_UNDEF=()
    while IFS= read -r s; do [[ -n "${s}" ]] && GL_SCE_UNDEF+=("${s}"); done < <(
        for a in "${GL_SDK}"/lib/*.a; do llvm-nm "${a}" 2>/dev/null || true; done \
        | awk '$1=="U"{print $2}' | grep -E '^sce[A-Za-z0-9_]+$' | sort -u || true)
    probe_name="--gl"
    (( GL_SMOKE )) && probe_name="--gl-smoke"
    (( GL_HDR_PROBE )) && probe_name="--gl-hdr-probe"
    ok "${probe_name}: ps5-opengl SDK at ${GL_SDK#"${REPO_ROOT}/"} (${#GL_LINK_LIBS[@]} link args, ${#GL_FORCE_UNDEF[@]} forced undefs, ${#GL_SCE_UNDEF[@]} sce imports)"
fi

load_sdk
need_cmd clang++ sha256sum python3 make llvm-nm prospero-clang prospero-clang++
: "${PS5_SYSROOT:=${PS5_PAYLOAD_SDK}/target}"
HB="${PS5_SYSROOT}/user/homebrew"

# Target toolchain: the SDK cross-compiler + the two flags the native-app link
# tail needs. NOT prospero-clang18 (which also forces -femulated-tls) - native
# TLS is the task-4 baseline; task 5 revisits emulated TLS if runtime needs it.
TCC="prospero-clang"
TCXX="prospero-clang++"
TFLAGS=(-fno-plt -fno-stack-protector -ffunction-sections -fdata-sections)

# --- Fixed loader / container constants -------------------------------------
# Cross-firmware-validated profile (6.02 + 12.70). Do NOT change these; they
# are what the 12.70 loader + ShadowMountPlus accepted for the Phase 1 gate.
MODULE_SDK=0x02000009
COMPANION_SDK=0x08050001
FSELF_MAGIC=0x1D3D154F

NATIVE="${REPO_ROOT}/tools/native-app"
EVO="${REPO_ROOT}/projects/evoplayer"
SCE_SYS="${EVO}/sce_sys"
PARAM="${SCE_SYS}/param.json"
APP_OUT="${OUTPUT_DIR}/app"
BUILD="${APP_OUT}/.build"
CLANG18="${NATIVE}/prospero-clang18"

LLD="$(command -v prospero-lld || echo "${PS5_PAYLOAD_SDK}/bin/prospero-lld")"
AR="$(command -v prospero-ar   || echo "${PS5_PAYLOAD_SDK}/bin/prospero-ar")"
need_file "${LLD}" "the PS5 payload SDK is missing prospero-lld"
need_file "${AR}"  "the PS5 payload SDK is missing prospero-ar"
need_file "${PARAM}" "projects/evoplayer/sce_sys/param.json is missing"

mkdir -p "${BUILD}/obj" "${BUILD}/host"

# ---------------------------------------------------------------------------
# 1. Validate param.json and read the identity out of it.
# ---------------------------------------------------------------------------
begin "validating param.json"
TITLE_ID="$(python3 - "${PARAM}" <<'PY'
import json, re, sys
v = json.load(open(sys.argv[1], encoding="utf-8"))
tid = v.get("titleId", "")
concept = v.get("conceptId", "")
content = v.get("contentId", "")
if not re.fullmatch(r"PPSA\d{5}", tid):                raise SystemExit("titleId must be PPSA + 5 digits")
if not re.fullmatch(r"\d{5}", concept):               raise SystemExit("conceptId must be 5 digits")
if not re.fullmatch(r"[A-Z]{2}\d{4}-PPSA\d{5}_00-[A-Z0-9]{16}", content) or tid not in content:
    raise SystemExit("contentId invalid or does not contain titleId")
if not re.fullmatch(r"\d{2}\.\d{3}\.\d{3}", v.get("contentVersion","")):  raise SystemExit("contentVersion must be NN.NNN.NNN")
if not re.fullmatch(r"\d{2}\.\d{2}", v.get("masterVersion","")):          raise SystemExit("masterVersion must be NN.NN")
s = v.get("downloadDataSize")
if isinstance(s, bool) or not isinstance(s, int) or s <= 0:
    raise SystemExit("downloadDataSize must be a positive integer (so /download0 is writable)")
if (v.get("applicationCategoryType"), v.get("contentBadgeType")) != (0, 1):
    raise SystemExit("expected a game module: applicationCategoryType 0, contentBadgeType 1")
intents = v.get("gameIntent", {}).get("permittedIntents", [])
if not any(i.get("intentType") == "launchActivity" for i in intents):
    raise SystemExit("game param.json must permit the launchActivity intent")
lp = v.get("localizedParameters", {})
if not lp.get(lp.get("defaultLanguage",""), {}).get("titleName","").strip():
    raise SystemExit("default-language titleName is empty")
print(tid)
PY
)" || die "param.json failed validation"
ok "title ${TITLE_ID}  (mode: ${MODE})"
APPDIR="${APP_OUT}/${TITLE_ID}"

# ---------------------------------------------------------------------------
# 2. Native-app dependency bootstrap (static zlib for the host converter).
# ---------------------------------------------------------------------------
begin "native-app dependencies"
eval "$("${SCRIPTS_DIR}/setup-native-app-deps.sh")"
need_file "${ZLIB_ARCHIVE:?}" "zlib bootstrap did not yield an archive"

# ---------------------------------------------------------------------------
# 3. Host converter tool.
# ---------------------------------------------------------------------------
begin "building host converter (ps5-native-tool)"
TOOL="${BUILD}/host/ps5-native-tool"
clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
    -I "${ZLIB_INCLUDE}" \
    "${NATIVE}/native_app_builder.cpp" "${NATIVE}/self_container.cpp" \
    "${NATIVE}/elf_object.cpp" "${NATIVE}/sce_module_writer.cpp" \
    "${ZLIB_ARCHIVE}" -o "${TOOL}"

# ---------------------------------------------------------------------------
# 4. Clean-room runtime shim (libc.prx).
# ---------------------------------------------------------------------------
LIBC_PRX="${NATIVE}/runtime/libc.prx"
build_libc() {
    begin "generating clean-room runtime shim (libc.prx)"
    local expected_signed
    expected_signed="$(awk '{print $1}' "${NATIVE}/runtime/libc.prx.sha256")"
    local work="${BUILD}/runtime-shim"
    mkdir -p "${work}"
    clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
        "${NATIVE}/libc_builder.cpp" -o "${work}/libc-builder"
    local copy
    for copy in a b; do
        "${work}/libc-builder" "${NATIVE}/runtime/api-surface.txt" \
            "${NATIVE}/runtime/imports.txt" "${work}/libc-${copy}.raw.elf"
    done
    cmp --silent "${work}/libc-a.raw.elf" "${work}/libc-b.raw.elf" \
        || die "libc_builder is not deterministic (raw ELF differs between runs)"
    for copy in a b; do
        "${TOOL}" self --sign --in "${work}/libc-${copy}.raw.elf" \
            --out "${work}/libc-${copy}.prx" >/dev/null
    done
    cmp --silent "${work}/libc-a.prx" "${work}/libc-b.prx" \
        || die "signed libc.prx differs between runs"
    local got
    got="$(sha256sum "${work}/libc-a.prx" | cut -d' ' -f1)"
    if [[ "${got}" != "${expected_signed}" ]]; then
        warn "generated libc.prx sha256 ${got}
       != pinned upstream ${expected_signed}. Expected once EVO re-harvests
       api-surface.txt (task 4); until then the build tail changed - stopping."
        die "libc.prx digest mismatch"
    fi
    local artifact forbidden
    for artifact in "${work}/libc-a.raw.elf" "${work}/libc-a.prx"; do
        grep -aFq BlackBearReloaded "${artifact}" \
            || die "generated runtime is missing its attribution marker"
        for forbidden in "W:/Build" "J013" "Prospero_Release" "sys/internal"; do
            grep -aFq "${forbidden}" "${artifact}" \
                && die "generated runtime contains forbidden text: ${forbidden}"
        done
    done
    cp "${work}/libc-a.prx" "${LIBC_PRX}"
    ok "libc.prx  (${got})"
}
if (( REBUILD_LIBC )) || [[ ! -f "${LIBC_PRX}" ]]; then
    build_libc
else
    ( cd "${NATIVE}/runtime" && sha256sum -c libc.prx.sha256 >/dev/null 2>&1 ) \
        || warn "existing libc.prx no longer matches libc.prx.sha256 (re-harvested?)"
    ok "libc.prx present"
fi

# ---------------------------------------------------------------------------
# 5. Compile the app object set.
# ---------------------------------------------------------------------------
OBJS=()
ARCHIVE_GROUP=()
CXX_RUNTIME=("${PS5_SYSROOT}/lib/libc++.a" "${PS5_SYSROOT}/lib/libc++abi.a" \
             "${PS5_SYSROOT}/lib/libunwind.a")

if [[ "${MODE}" == "probe" ]]; then
    APP_NAME="sandbox_probe"
    begin "compiling ${APP_NAME}"
    PROBE_SRCS=("${REPO_ROOT}/projects/sandbox_probe/main.c"
               "${REPO_ROOT}/projects/common/src/evo_notify.c")
    PROBE_DEFS=()
    for src in "${PROBE_SRCS[@]}"; do
        obj="${BUILD}/obj/$(echo "${src#"${REPO_ROOT}/"}" | tr '/.' '__').o"
        "${TCC}" -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
            "${TFLAGS[@]}" ${PROBE_DEFS[@]+"${PROBE_DEFS[@]}"} \
            -I"${REPO_ROOT}/projects/common/include" \
            -I"${REPO_ROOT}/projects/evoplayer/include" \
            -I"${REPO_ROOT}/projects/evoplayer/media/include" \
            -c "${src}" -o "${obj}"
        OBJS+=("${obj}")
    done
else
    APP_NAME="EVO Player (FFmpeg-software)"
    begin "compiling ${APP_NAME} objects (${TCC} ${TFLAGS[*]})"
    # EVO's Makefile owns the source list, include paths and -D flags; we only
    # add the native-app link-tail flags via EXTRA_CFLAGS.
    # EVO_APP_MODULE: routes data paths to /download0/evoplayer and directory
    # enumeration through getdents (opendir fails EPERM in the sandbox).
    # Build fingerprint — main()'s FIRST notification, so a stale ShadowMount /
    # cached mount is caught immediately instead of costing a console session.
    # Written to a generated header to sidestep -D string quoting through the
    # make -> /bin/sh recipe chain.
    BUILD_SHA="$(git -C "${EVO}" rev-parse --short=8 HEAD 2>/dev/null || echo unknown)"
    git -C "${EVO}" diff --quiet -- "${EVO}" 2>/dev/null || BUILD_SHA="${BUILD_SHA}-dirty"
    printf '#pragma once\n#define EVO_BUILD_ID "%s_%s"\n' \
        "${BUILD_SHA}" "$(date -u +%m%d-%H%M)" > "${EVO}/include/evo_build_id.h"
    ok "build id $(sed -n 's/.*"\(.*\)".*/\1/p' "${EVO}/include/evo_build_id.h")"
    # main.c bakes EVO_BUILD_ID in but the Makefile tracks source mtimes, not
    # this generated header - drop main.o so the id on screen is always current.
    rm -f "${EVO}/main.o"
    APP_DEFS="-DEVO_APP_MODULE=1 -DEVO_HAVE_BUILD_ID=1"
    # --usb-remote: the scriptable dev remote (evo_usb_remote.c) — the
    # /mnt/usb0/evo_status snapshot + the evo_cmd command channel. Off by
    # default so a release eboot never touches the user's USB stick per frame.
    # The diagnostic log (/mnt/usb0/evo.log) is written by every build.
    (( USB_REMOTE )) && APP_DEFS+=" -DEVO_USB_REMOTE=1"
    # --breadcrumbs (#51): bring back the on-screen boot-trace notification
    # popups (evo_bt / evo_boot_log). Off by default - klog
    # (tools/klog.sh) carries the same lines unconditionally in the app
    # module now, so the popups are only useful watching the TV without klog.
    (( BREADCRUMBS )) && APP_DEFS+=" -DEVO_BOOT_TRACE_POPUP=1"
    # --gl-smoke (#77): EVO_GL_SMOKE gate in main.c + the pp_gl_smoke/pp_gl_fatal
    # objects (the Makefile adds them to PP_SRCS when GL_SMOKE=1).
    (( GL_SMOKE )) && APP_DEFS+=" -DEVO_GL_SMOKE=1"
    # --gl-hdr-probe (Task 1): EVO_GL_HDR_PROBE gate in main.c + pp_gl_fatal.c.
    (( GL_HDR_PROBE )) && APP_DEFS+=" -DEVO_GL_HDR_PROBE=1"
    # --native-secondary (#41): HEVC + VP9 resident sceVideodec2 decoders, at
    # 4K since 2026-09-11 — both hardware-verified (evo_vdec_native.c has the
    # full evidence). --no-native-secondary / --no-native-secondary-4k are the
    # rollback escape hatches.
    (( NATIVE_SECONDARY ))       && APP_DEFS+=" -DEVO_VDEC_NATIVE_SECONDARY=1"
    (( NATIVE_SECONDARY_4K ))    && APP_DEFS+=" -DEVO_VDEC_NATIVE_SECONDARY_4K=1"
    (( NO_NATIVE_SECONDARY ))    && APP_DEFS+=" -DEVO_VDEC_NATIVE_SECONDARY=0"
    (( NO_NATIVE_SECONDARY_4K )) && APP_DEFS+=" -DEVO_VDEC_NATIVE_SECONDARY_4K=0"
    (( NATIVE_10BIT ))        && APP_DEFS+=" -DEVO_VDEC_NATIVE_10BIT=1"
    # --gl (#79 GL-3): EVO_GL_DEVICE gate in main.c + evo_gl_context_device.cpp +
    # pp_gl_fatal.c (the Makefile adds them when GL_DEVICE=1).
    (( GL_DEVICE )) && APP_DEFS+=" -DEVO_GL_DEVICE=1"
    # --agc: EVO_AGC_DEVICE gate in main.c + evo_agc_* + evo_rmlui_render_agc.cpp
    (( AGC_DEVICE )) && APP_DEFS+=" -DEVO_AGC_DEVICE=1"
    rm -f "${EVO}/include/evo_autoplay.h"

    # The Makefile tracks sources, NOT the -D flag set. The app-module defines
    # (EVO_APP_MODULE, EVO_BOOT_TRACE_POPUP, ...) differ from build-evoplayer.sh's, so
    # `make objects` would silently reuse payload .o files - which is exactly
    # how three console sessions shipped an eboot with none of the app-module
    # code. Force a clean object build whenever the flag set changed.
    STAMP="${BUILD}/app-cflags.stamp"
    WANT="${TFLAGS[*]} ${APP_DEFS} ${GL_PUBLIC_CFLAGS}"
    if [[ ! -f "${STAMP}" || "$(cat "${STAMP}" 2>/dev/null)" != "${WANT}" ]]; then
        begin "app-module flags changed - clean rebuild"
        make -C "${EVO}" clean >/dev/null 2>&1 || true
        printf '%s' "${WANT}" > "${STAMP}"
    fi

    # #60: regenerate the embedded RmlUi asset bundle (rml/rcss/fonts/icons)
    # so the .ffpfsc is always built from whatever assets/ currently holds -
    # never from a stale evo_rmlui_bundle_data.cpp left over from a previous
    # commit. See tools/bundle_rml_assets.py and evo_rmlui_fileinterface.cpp.
    begin "bundling RmlUi assets into evo_rmlui_bundle_data.cpp"
    python3 "${REPO_ROOT}/tools/bundle_rml_assets.py"

    make -C "${EVO}" objects -j"$(nproc)" \
        CC="${TCC}" CXX="${TCXX}" \
        AGC_DEVICE="${AGC_DEVICE}" \
        GL_SMOKE="${GL_SMOKE}" GL_DEVICE="${GL_DEVICE}" GL_HDR_PROBE="${GL_HDR_PROBE}" \
        EXTRA_CFLAGS="${WANT}" \
        > "${BUILD}/compile.log" 2>&1 || {
            echo "--- last 40 lines of compile.log ---"
            tail -40 "${BUILD}/compile.log" | sed 's/^/  /'
            die "object compile failed. Full log: ${BUILD#"${REPO_ROOT}/"}/compile.log"
        }
    while read -r rel; do
        OBJS+=("${EVO}/${rel}")
    done < <(make -C "${EVO}" -s AGC_DEVICE="${AGC_DEVICE}" GL_SMOKE="${GL_SMOKE}" GL_DEVICE="${GL_DEVICE}" GL_HDR_PROBE="${GL_HDR_PROBE}" print-objects | tr ' ' '\n' | grep -E '\.o$')
    ok "compiled ${#OBJS[@]} objects"

    # #68: RmlUi puts only `3 + R/6` points on a corner arc, so a 20 px radius is
    # a 5-segment polygon that sags a quarter-pixel inside the true circle -
    # visible faceting on every rounded card, and a geometry error no rasteriser
    # or MSAA can undo. ui_rml/src/rmlui_patch/ carries that one RmlUi TU with a
    # finer GetNumPoints(); compile it here and swap it over the matching member
    # of a build-local COPY of the pacbrew archive (same RmlUi 6.2 release - the
    # public headers are byte-identical - so this is a like-for-like member
    # replacement, and the sysroot archive is never touched).
    # See ui_rml/src/rmlui_patch/VENDORED.md.
    begin "patching librmlui.a corner tessellation (#68)"
    RML_PATCH_O="${BUILD}/obj/GeometryBackgroundBorder.cpp.o"
    "${TCXX}" -std=c++17 -O2 "${TFLAGS[@]}" \
        -I"${HB}/include" \
        -c "${EVO}/ui_rml/src/rmlui_patch/GeometryBackgroundBorder.cpp" \
        -o "${RML_PATCH_O}"
    RML_A="${BUILD}/librmlui.a"
    cp -f "${HB}/lib/librmlui.a" "${RML_A}"
    llvm-ar r "${RML_A}" "${RML_PATCH_O}"
    # The swap is only a swap if the member name matched - a typo would silently
    # ADD a member and leave upstream's tessellation linked ahead of it.
    [[ "$(llvm-ar t "${RML_A}" | grep -c '^GeometryBackgroundBorder\.cpp\.o$')" == "1" ]] \
        || die "librmlui.a member swap failed - GeometryBackgroundBorder.cpp.o not unique"
    ok "librmlui.a patched"
    ARCHIVE_GROUP+=("${RML_A}")

    # Static archives EVO links (Makefile LIBS + build-evoplayer.sh transitive
    # set). Order-independent inside the group.
    for a in libSDL2 \
             libavformat libavcodec libswresample libavutil libswscale \
             libass libfreetype libharfbuzz libharfbuzz-subset libfribidi \
             libpng16 libsamplerate libssl libcrypto libiconv \
             libz libbz2 liblzma libzstd libm; do
        f="${HB}/lib/${a}.a"
        need_file "${f}" "expected port archive missing: ${a}.a (pacbrew sysroot incomplete)"
        ARCHIVE_GROUP+=("${f}")
    done
    # --gl / --gl-smoke: ps5-opengl-core33 (Mesa + PS5 Gallium + PSBC). The .mk
    # names them via -L/-l; add those as raw link args INSIDE the archive group
    # so the circular Mesa<->driver refs resolve.
    (( GL_LINK )) && ARCHIVE_GROUP+=("${GL_LINK_LIBS[@]}")
fi

# ---------------------------------------------------------------------------
# 6. CRT + C++ allocation runtime + fake libpthread.a.
# ---------------------------------------------------------------------------
begin "compiling app CRT + allocation runtime + libc gap fillers"
"${TCXX}" -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti "${TFLAGS[@]}" \
    -c "${NATIVE}/app_crt.cpp" -o "${BUILD}/obj/app_crt.o"
"${TCXX}" -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti "${TFLAGS[@]}" \
    -c "${NATIVE}/app_cpp_runtime.cpp" -o "${BUILD}/obj/app_cpp_runtime.o"
"${AR}" rcs "${BUILD}/obj/libpthread.a" "${BUILD}/obj/app_cpp_runtime.o"

# stubs/libc_ext.c: real C-locale implementations of the ~45 FreeBSD libc names
# (xlocale *_l, _setjmp/_longjmp, gmtime_r, nl_langinfo, dladdr, catgets...) that
# neither the SDK stub nor the console's libSceLibcInternal export. Compiled IN
# as local defs so libc++'s iostream/codecvt static init cannot call a NULL
# import (that was the first-launch SIGSEGV at rip=0 in DoIOSInit).
LIBC_EXT_O=""
MALLOC_SHIM_O=""
if [[ "${MODE}" == "player" ]]; then
    LIBC_EXT_O="${BUILD}/obj/libc_ext.o"
    "${TCC}" -std=gnu11 -O2 -w "${TFLAGS[@]}" \
        -c "${NATIVE}/stubs/libc_ext.c" -o "${LIBC_EXT_O}"
    # malloc interposer: the clean-room libc.prx heap is bounded and fills up
    # (hardware 2026-09-02); route every allocation to an mmap-backed allocator.
    MALLOC_SHIM_O="${BUILD}/obj/malloc_shim.o"
    "${TCC}" -std=gnu11 -O2 -w "${TFLAGS[@]}" \
        -c "${NATIVE}/stubs/malloc_shim.c" -o "${MALLOC_SHIM_O}"
fi

# ---------------------------------------------------------------------------
# 6b. PRX import stubs for system modules the SDK ships no .so for.
#     Each tools/native-app/stubs/prx/<name>.syms -> a tiny ELF .so with
#     SONAME <name>.sprx and one empty FUNC per symbol, linked POSITIONALLY
#     (unconditional DT_NEEDED, like ProsperoLight) and passed to
#     native_app_builder as --stub. The converter computes the Sony NID from
#     the plain name and the loader auto-loads the .sprx at process start.
#     Only built for modules a probe actually uses - an unconditional NEEDED on
#     a module the loader refuses would brick the boot.
#     Fixes the hardware-proven wall: a fake-signed module cannot
#     sceKernelLoadStartModule an undeclared system PRX. See the prx/README.
# ---------------------------------------------------------------------------
PRX_STUB_SOS=()
PRX_STUB_SRC="${NATIVE}/stubs/prx"
PRX_STUB_WANT=()
# libSceVideodec2's own startup load needs the GPU driver stack present
# (sceVideodec2AllocateComputeQueue allocates a GPU compute queue). ProsperoLight
# links libSceAgc + libSceAgcDriver, which pull in libSceGnmDriver and satisfy
# that; EVO must do the same or libSceVideodec2 loads broken. ps5-opengl's
# Gallium driver also imports sceAgc* / sceAgcDriver* directly, and --gl is the
# only app-module build - the libSceAgc/libSceAgcDriver .syms are comment-only
# now (GL-6 deleted pp_agc.c) and the actual symbol list is populated from the
# ps5-opengl archives (GL_SCE_UNDEF) in the augmentation block below.
# The native decode backend (media/src/evo_vdec_native.c, #31) is compiled into
# every MODE == player eboot, so libSceVideodec2 + its GPU-driver deps must be
# positional DT_NEEDED.
# #34: the native IME keyboard. libSceImeDialog has a real SDK stub .so and
# resolves through the link tail; libSceCommonDialog has none, and
# sceCommonDialogInitialize() must run before any common dialog will start.
if [[ "${MODE}" == "player" ]]; then
    PRX_STUB_WANT+=(libSceVideodec2 libSceAgc libSceAgcDriver libSceCommonDialog)
fi
if (( ${#PRX_STUB_WANT[@]} )); then
    begin "building PRX import stubs"
    mkdir -p "${BUILD}/stubs"

    # Every name in a .syms becomes a POSITIONAL (unconditional DT_NEEDED) import
    # from <module>.prx. If nothing in the objects actually references it, it is a
    # dead import: the loader still has to bind its NID on the console, and if that
    # NID is absent from the firmware's .sprx it REJECTS THE WHOLE MODULE -> the
    # app "crashes" before main() with CE-108255-1 and no log. This cost a full
    # session once (sceAgcDcbDrawIndexOffset, #28). Refuse to build a stub that
    # carries a symbol no object imports; annotate a deliberate one with
    # `# keep: <reason>` on its line.
    OBJ_UNDEF="${BUILD}/obj-undef.txt"
    : > "${OBJ_UNDEF}"
    for o in "${OBJS[@]}" "${LIBC_EXT_O}" "${MALLOC_SHIM_O}"; do
        [[ -n "${o}" && -f "${o}" ]] && llvm-nm -u "${o}" 2>/dev/null \
            | grep -oE '\bsce[A-Za-z0-9_]+' >> "${OBJ_UNDEF}" || true
    done
    # --gl / --gl-smoke: the ps5-opengl archives import these too (harvested above).
    (( GL_LINK )) && printf '%s\n' "${GL_SCE_UNDEF[@]}" >> "${OBJ_UNDEF}"
    sort -u -o "${OBJ_UNDEF}" "${OBJ_UNDEF}"

    dead_total=0
    for base in "${PRX_STUB_WANT[@]}"; do
        syms="${PRX_STUB_SRC}/${base}.syms"
        need_file "${syms}" "missing PRX stub symbol list: ${base}.syms"
        so="${BUILD}/stubs/${base}.so"
        csrc="${BUILD}/stubs/${base}.c"
        # (a comment-only .syms - libSceAgc/libSceAgcDriver post-GL-6 - greps to
        #  nothing; `|| true` so the empty result isn't a pipeline failure.)
        { grep -vE '^\s*(#|$)' "${syms}" || true; } | awk '{print "void " $1 "(void){}"}' > "${csrc}"
        # --gl / --gl-smoke / --agc: add the sceAgc* / sceAgcDriver* names that
        # this .syms doesn't already carry. Routed by prefix; sceVideoOut* etc.
        # resolve from target/lib/*.so and are left alone.
        if (( GL_LINK || AGC_DEVICE )) && [[ "${base}" == libSceAgc || "${base}" == libSceAgcDriver ]]; then
            existing="$({ grep -vE '^\s*(#|$)' "${syms}" || true; } | awk '{print $1}')"
            source_syms=()
            if (( GL_LINK )); then
                source_syms=("${GL_SCE_UNDEF[@]}")
            else
                while IFS= read -r s; do [[ -n "${s}" ]] && source_syms+=("${s}"); done < "${OBJ_UNDEF}"
            fi
            for s in "${source_syms[@]}"; do
                if [[ "${base}" == libSceAgcDriver ]]; then
                    [[ "${s}" == sceAgcDriver* ]] || continue
                else
                    [[ "${s}" == sceAgc* && "${s}" != sceAgcDriver* ]] || continue
                fi
                grep -qxF "${s}" <<<"${existing}" && continue
                echo "void ${s}(void){}" >> "${csrc}"
                echo "     + ${base}: ${s}"
            done
        fi

        # dead-import guard (skip lines annotated `# keep:`)
        while read -r sym rest; do
            [[ "${rest}" == *"# keep:"* ]] && continue
            grep -qxF "${sym}" "${OBJ_UNDEF}" && continue
            warn "PRX stub ${base}.syms: '${sym}' is not imported by any object"
            echo "       -> a dead positional import; if its NID is absent on"
            echo "          firmware the loader rejects the module at load."
            echo "          Remove it, or annotate the line with '# keep: <why>'."
            dead_total=$((dead_total + 1))
        done < <(grep -vE '^\s*(#|$)' "${syms}" || true)

        "${TCC}" -shared -nostdlib -nodefaultlibs -fPIC \
            -Wl,-soname,"${base}.sprx" -o "${so}" "${csrc}"
        PRX_STUB_SOS+=("${so}")
        printf '     %-22s %s syms\n' "${base}.sprx" "$(wc -l < "${csrc}")"
    done
    (( dead_total )) && die "${dead_total} dead PRX import(s) - see above (would brick the app at load)"
    ok "built ${#PRX_STUB_SOS[@]} PRX import stubs"
fi

# ---------------------------------------------------------------------------
# 7. Link the intermediate PS5 PIE.
# ---------------------------------------------------------------------------
begin "linking intermediate PIE"
LINK_LOG="${BUILD}/link.log"
LINK_INPUTS=()
[[ -n "${MALLOC_SHIM_O}" ]] && LINK_INPUTS+=("${MALLOC_SHIM_O}")
LINK_INPUTS+=("${BUILD}/obj/app_crt.o" "${BUILD}/obj/app_cpp_runtime.o")
[[ -n "${LIBC_EXT_O}" ]] && LINK_INPUTS+=("${LIBC_EXT_O}")
LINK_INPUTS+=("${OBJS[@]}")
# PRX import stubs: POSITIONAL (not --as-needed), matching ProsperoLight's
# tools/build.sh exactly. An --as-needed-derived DT_NEEDED for a system PRX
# would not bind correctly for a fake-signed module (the sceAvPlayerInit /
# sceVideodec2* first-call crashes traced to this).
(( ${#PRX_STUB_SOS[@]} )) && LINK_INPUTS+=("${PRX_STUB_SOS[@]}")
(( ${#ARCHIVE_GROUP[@]} )) && LINK_INPUTS+=(--start-group "${ARCHIVE_GROUP[@]}" --end-group)
(( ${#ARCHIVE_GROUP[@]} )) && LINK_INPUTS+=("${CXX_RUNTIME[@]}")

STUBDIR="${PS5_SYSROOT}/lib"

# The static SDK libc.a supplies __emutls_get_address (emulated-TLS runtime,
# forced by the SDK clang) plus real C-locale implementations of the xlocale
# *_l family, gmtime_r, nl_langinfo, __assert, ... that neither the SDK stub nor
# the console libSceLibcInternal export. Linked last, in its own group so its
# members resolve each other; archive semantics keep malloc/stdio/etc. bound to
# the .so stubs (and, on device, the runtime shim's heap table).
LINK_TAIL=(--as-needed "${STUBDIR}"/*.so)
(( ${#PRX_STUB_SOS[@]} )) && LINK_TAIL+=("${PRX_STUB_SOS[@]}")
[[ "${MODE}" == "player" ]] && \
    LINK_TAIL+=(--start-group "${PS5_SYSROOT}/lib/libc.a" --end-group)

# --gl / --gl-smoke: --wrap=_Exit routes any stray _Exit (past the 4 patched
# sites) through pp_gl_fatal.c; -u ps5_agc_gate2_run keeps the ps5-opengl submit
# entry against --gc-sections (mirrors toolchain/ps5-opengl-core33.mk).
GL_LINK_EXTRA=()
if (( GL_LINK )); then
    GL_LINK_EXTRA+=(--wrap=_Exit "${GL_FORCE_UNDEF[@]}")
fi

LINK_RC=0
if ! "${LLD}" -T "${NATIVE}/ps5-pie.ld" --eh-frame-hdr \
    --version-script "${NATIVE}/app-symbols.map" \
    --exclude-libs=ALL --error-limit=0 \
    ${GL_LINK_EXTRA[@]+"${GL_LINK_EXTRA[@]}"} \
    -L "${BUILD}/obj" \
    -e _start -o "${BUILD}/llvm-pie.elf" \
    "${LINK_INPUTS[@]}" \
    "${LINK_TAIL[@]}" \
    2> "${LINK_LOG}"; then
    LINK_RC=1
fi

if (( LINK_RC != 0 )); then
    UND="${BUILD}/undefined-symbols.txt"
    grep -oE "undefined symbol: .*" "${LINK_LOG}" | sed 's/^undefined symbol: //' \
        | sort -u > "${UND}" || true
    echo ""
    warn "intermediate link failed (rc ${LINK_RC})."
    if [[ -s "${UND}" ]]; then
        n=$(wc -l < "${UND}")
        echo "   ${n} distinct undefined symbols -> ${UND#"${REPO_ROOT}/"}"
        echo "   sample:"
        head -30 "${UND}" | sed 's/^/     /'
        echo ""
        echo "   Next: classify these (SCE stub gap vs libc re-harvest), then either"
        echo "   add tools/native-app/stubs/*.c or extend runtime/api-surface.txt."
        echo "   NID form for the re-harvest:"
        while read -r s; do printf '     %-34s %s\n' "$s" "$(prospero-nid "$s" 2>/dev/null || echo '?')"; done \
            < <(head -20 "${UND}")
    else
        echo "--- link.log tail ---"
        tail -30 "${LINK_LOG}" | sed 's/^/  /'
    fi
    die "link incomplete - see above (this is expected on the first task-4 pass)"
fi
ok "linked  $(stat -c %s "${BUILD}/llvm-pie.elf") bytes"

# Record what the PIE imports, for the api-surface diff.
llvm-nm -u "${BUILD}/llvm-pie.elf" 2>/dev/null | awk '{print $NF}' | sort -u \
    > "${BUILD}/pie-imports.txt" || true

# ---------------------------------------------------------------------------
# 8. Convert LLVM PIE -> PS5 module, then sign to FSELF.
# ---------------------------------------------------------------------------
begin "converting to PS5 module + signing"
CONV_STUB_ARGS=()
for so in ${PRX_STUB_SOS[@]+"${PRX_STUB_SOS[@]}"}; do CONV_STUB_ARGS+=(--stub "${so}"); done
"${TOOL}" link --in "${BUILD}/llvm-pie.elf" --out "${BUILD}/eboot.elf" \
    --stub-dir "${STUBDIR}" ${CONV_STUB_ARGS[@]+"${CONV_STUB_ARGS[@]}"} \
    --module-sdk "${MODULE_SDK}" --companion-sdk "${COMPANION_SDK}" \
    --file-name eboot.elf

rm -rf -- "${APPDIR}"
mkdir -p "${APPDIR}/sce_sys" "${APPDIR}/sce_module"
"${TOOL}" self --sign --in "${BUILD}/eboot.elf" --out "${APPDIR}/eboot.bin" \
    --magic "${FSELF_MAGIC}"

# ---------------------------------------------------------------------------
# 9. Assemble the deployment folder.
# ---------------------------------------------------------------------------
cp "${PARAM}" "${APPDIR}/sce_sys/param.json"
cp "${LIBC_PRX}" "${APPDIR}/sce_module/libc.prx"
for asset in icon0.png pic0.png pic1.png snd0.at9; do
    [[ -f "${SCE_SYS}/${asset}" ]] && cp "${SCE_SYS}/${asset}" "${APPDIR}/sce_sys/${asset}"
done
if [[ "${MODE}" == "player" && -d "${EVO}/assets" ]]; then
    cp -a "${EVO}/assets" "${APPDIR}/assets"
fi

begin "inspecting signed containers"
"${TOOL}" self --inspect --file "${APPDIR}/eboot.bin"

# ---------------------------------------------------------------------------
# 10. Optional: PFS-pack the app folder into a single .ffpfsc image, matching
#     ProsperoLight's packaging exactly (same MkPFS, same layout). The eboot,
#     param.json and libc.prx are byte-identical to the folder above - this is
#     only a different container on /data/homebrew.
# ---------------------------------------------------------------------------
if (( FFPFSC )); then
    begin "PFS-packing ${TITLE_ID}.ffpfsc (MkPFS)"
    if MKPFS="$("${SCRIPTS_DIR}/setup-pfs-tool.sh")"; then
        IMG="${APP_OUT}/${TITLE_ID}.ffpfsc"
        rm -f -- "${IMG}"
        "${MKPFS}" pack folder --no-adjust-output-file-extension \
            --version PS5 --verify "${APPDIR}" "${IMG}" \
            && ok "ffpfsc: ${IMG#"${REPO_ROOT}/"}  ($(stat -c %s "${IMG}") bytes)" \
            || warn "MkPFS pack failed - the folder output above is still usable"
    else
        warn "MkPFS unavailable (needs git + python3-venv + network on first run).
       Skipping .ffpfsc; deploy the folder with scripts/deploy-app.sh."
    fi
fi

echo ""
ok "app module: ${APPDIR#"${REPO_ROOT}/"}/"
find "${APPDIR}" -type f -printf '     %-42P  %s bytes\n' | sort
echo ""
echo "   Deploy + launch:"
echo "     ./scripts/deploy-app.sh          # FTP -> /data/homebrew/${TITLE_ID}/"
echo "     # then mount + launch from the Games row via ShadowMountPlus"
echo "   Do NOT stack launches - PS button to close before rebuilding."
