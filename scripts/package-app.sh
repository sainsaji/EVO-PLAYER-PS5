#!/usr/bin/env bash
# =============================================================================
# scripts/package-app.sh - build EVO Player as a game-category app module.
#
#   ./scripts/package-app.sh                  build the full FFmpeg-software
#                                             player (Phase 1b task 4+)
#   ./scripts/package-app.sh --probe          build the sandbox probe instead
#   ./scripts/package-app.sh --rebuild-libc   force-regenerate the runtime shim
#   ./scripts/package-app.sh --agc-probe      + boot-time sceAgc reachability
#                                             recon (GPU rendering Step 2 gate)
#   ./scripts/package-app.sh --avplayer-probe + libSceAvPlayer gate (Route A — DEAD)
#   ./scripts/package-app.sh --videodec2-probe + sceVideodec2 gate (Route B, the one)
#   ./scripts/package-app.sh --ffpfsc         also emit a PFS image, like
#                                             ProsperoLight (needs MkPFS)
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
AGC_PROBE=0
AVPLAYER_PROBE=0
VIDEODEC2_PROBE=0
FFPFSC=0
while (( $# )); do
    case "$1" in
        --probe)        MODE="probe" ;;
        --player)       MODE="player" ;;
        --rebuild-libc) REBUILD_LIBC=1 ;;
        --agc-probe)    AGC_PROBE=1 ;;
        --avplayer-probe) AVPLAYER_PROBE=1 ;;
        --videodec2-probe) VIDEODEC2_PROBE=1 ;;
        --ffpfsc)       FFPFSC=1 ;;
        -h|--help)      sed -n '2,18p' "$0"; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

if ! in_container; then
    FWD=(--"${MODE}")
    (( REBUILD_LIBC )) && FWD+=(--rebuild-libc)
    (( AGC_PROBE ))    && FWD+=(--agc-probe)
    (( AVPLAYER_PROBE )) && FWD+=(--avplayer-probe)
    (( VIDEODEC2_PROBE )) && FWD+=(--videodec2-probe)
    (( FFPFSC ))       && FWD+=(--ffpfsc)
    reexec_in_container "package-app.sh" "${FWD[@]}"
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
    for src in "${REPO_ROOT}/projects/sandbox_probe/main.c" \
               "${REPO_ROOT}/projects/common/src/evo_notify.c"; do
        obj="${BUILD}/obj/$(echo "${src#"${REPO_ROOT}/"}" | tr '/.' '__').o"
        "${TCC}" -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
            "${TFLAGS[@]}" \
            -I"${REPO_ROOT}/projects/common/include" -c "${src}" -o "${obj}"
        OBJS+=("${obj}")
    done
else
    APP_NAME="EVO Player (FFmpeg-software)"
    begin "compiling ${APP_NAME} objects (${TCC} ${TFLAGS[*]})"
    # EVO's Makefile owns the source list, include paths and -D flags; we only
    # add the native-app link-tail flags via EXTRA_CFLAGS.
    # EVO_BOOT_TRACE: system-notification breadcrumbs through main()'s init
    # (only channel visible before VideoOut). Drop once milestone 1 is signed.
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
    APP_DEFS="-DEVO_BOOT_TRACE=1 -DEVO_APP_MODULE=1 -DEVO_HAVE_BUILD_ID=1"
    (( AGC_PROBE )) && APP_DEFS+=" -DEVO_AGC_PROBE=1"
    (( AVPLAYER_PROBE )) && APP_DEFS+=" -DEVO_AVPLAYER_PROBE=1"
    (( VIDEODEC2_PROBE )) && APP_DEFS+=" -DEVO_VIDEODEC2_PROBE=1"

    # The Makefile tracks sources, NOT the -D flag set. The app-module defines
    # (EVO_APP_MODULE, EVO_BOOT_TRACE, ...) differ from build-evoplayer.sh's, so
    # `make objects` would silently reuse payload .o files - which is exactly
    # how three console sessions shipped an eboot with none of the app-module
    # code. Force a clean object build whenever the flag set changed.
    STAMP="${BUILD}/app-cflags.stamp"
    WANT="${TFLAGS[*]} ${APP_DEFS}"
    if [[ ! -f "${STAMP}" || "$(cat "${STAMP}" 2>/dev/null)" != "${WANT}" ]]; then
        begin "app-module flags changed - clean rebuild"
        make -C "${EVO}" clean >/dev/null 2>&1 || true
        printf '%s' "${WANT}" > "${STAMP}"
    fi

    make -C "${EVO}" objects -j"$(nproc)" \
        CC="${TCC}" CXX="${TCXX}" \
        EXTRA_CFLAGS="${WANT}" \
        > "${BUILD}/compile.log" 2>&1 || {
            echo "--- last 40 lines of compile.log ---"
            tail -40 "${BUILD}/compile.log" | sed 's/^/  /'
            die "object compile failed. Full log: ${BUILD#"${REPO_ROOT}/"}/compile.log"
        }
    while read -r rel; do
        OBJS+=("${EVO}/${rel}")
    done < <(make -C "${EVO}" -s print-objects | tr ' ' '\n' | grep -E '\.o$')
    ok "compiled ${#OBJS[@]} objects"

    # Static archives EVO links (Makefile LIBS + build-evoplayer.sh transitive
    # set). Order-independent inside the group.
    for a in librmlui libSDL2 \
             libavformat libavcodec libswresample libavutil libswscale \
             libass libfreetype libharfbuzz libharfbuzz-subset libfribidi \
             libpng16 libsamplerate libssl libcrypto libiconv \
             libz libbz2 liblzma libzstd libm; do
        f="${HB}/lib/${a}.a"
        need_file "${f}" "expected port archive missing: ${a}.a (pacbrew sysroot incomplete)"
        ARCHIVE_GROUP+=("${f}")
    done
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
#     SONAME <name>.sprx and one empty FUNC per symbol. Linked --as-needed
#     (only becomes a real NEEDED entry if EVO references a symbol) and passed
#     to native_app_builder as --stub. The converter computes the Sony NID
#     from the plain name and the loader auto-loads the .sprx at start.
#     Fixes the hardware-proven wall: a fake-signed module cannot
#     sceKernelLoadStartModule an undeclared system PRX. See the prx/README.
# ---------------------------------------------------------------------------
PRX_STUB_SOS=()
PRX_STUB_SRC="${NATIVE}/stubs/prx"
if compgen -G "${PRX_STUB_SRC}/*.syms" > /dev/null; then
    begin "building PRX import stubs"
    mkdir -p "${BUILD}/stubs"
    for syms in "${PRX_STUB_SRC}"/*.syms; do
        base="$(basename "${syms}" .syms)"
        so="${BUILD}/stubs/${base}.so"
        csrc="${BUILD}/stubs/${base}.c"
        grep -vE '^\s*(#|$)' "${syms}" | awk '{print "void " $1 "(void){}"}' > "${csrc}"
        "${TCC}" -shared -nostdlib -nodefaultlibs -fPIC \
            -Wl,-soname,"${base}.sprx" -o "${so}" "${csrc}"
        PRX_STUB_SOS+=("${so}")
        printf '     %-22s %s syms\n' "${base}.sprx" "$(wc -l < "${csrc}")"
    done
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

LINK_RC=0
if ! "${LLD}" -T "${NATIVE}/ps5-pie.ld" --eh-frame-hdr \
    --version-script "${NATIVE}/app-symbols.map" \
    --exclude-libs=ALL --error-limit=0 \
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
