#!/usr/bin/env bash
# =============================================================================
# scripts/package-pkg.sh - package an ELF for installation on the PS5.
#
#   ./scripts/package-pkg.sh --format homebrew output/elf/hello_world.elf
#   ./scripts/package-pkg.sh --format app      output/elf/hello_world.elf \
#                            --title-id FAKE00001 --title "EVO Player"
#
# ---------------------------------------------------------------------------
# WHAT "PKG" ACTUALLY MEANS ON PS5 - read before assuming anything
#
# The brief warns not to assume a PS4 PKG tool works here. It does not, and the
# difference is structural, not cosmetic:
#     PS4 applications are described by  sce_sys/param.sfo  (binary SFO)
#     PS5 applications are described by  sce_sys/param.json (JSON)
# A PS4 fPKG tool emits the wrong metadata format entirely.
#
# There are three genuinely different distribution routes. This script
# implements the two that need no proprietary Sony tooling.
#
#   1. HOMEBREW BUNDLE            [--format homebrew]  <-- default, recommended
#      ps5-payload-websrv scans /data/homebrew, /mnt/usb*/homebrew and
#      /mnt/ext*/homebrew for directories containing:
#          <name>/eboot.elf            the payload
#          <name>/sce_sys/icon0.png    icon shown in the launcher
#          <name>/homebrew.js          optional UI extension (argv, options)
#      No signing, no encryption, no PKG at all. Copy the directory to a USB
#      stick and it appears in the websrv launcher. This is how essentially
#      all ps5-payload-dev homebrew is actually shipped.
#
#   2. FAKE-SELF APP             [--format app]
#      Registers a real system application with a TITLE_ID so it appears on
#      the PS5 home screen. The SDK implements this openly in
#      samples/install_app: make_fself.py converts the ELF to eboot.bin with a
#      fake authinfo blob, and a small payload calls sceAppInstUtil to
#      register the title. Files are pushed over FTP (ps5-payload-ftpsrv,
#      port 2121) into /system_ex/app/<TITLE_ID>/ and /user/app/<TITLE_ID>/.
#      This is what ProsperoPlayer's "Media tile" feature is doing.
#
#   3. TRUE SIGNED fPKG          NOT IMPLEMENTED - and cannot be, here
#      Building a real .pkg requires Sony's prospero-pub-cmd from the official
#      Prospero SDK. That is proprietary and must never be vendored into this
#      repository. Third-party GUI builders exist (e.g. LibProsperoPKG /
#      PS-Multi-Tools) but they wrap the same proprietary tool. If you have a
#      legitimate licensed copy, put it in proprietary/ and see
#      docs/packaging.md - this script will detect and use it, and will tell
#      you clearly when it is absent rather than silently producing something
#      that is not a PKG.
#
# PKG packaging is deliberately NOT a dependency of the ELF workflow: nothing
# in scripts/build.sh or deploy.sh calls this.
# ---------------------------------------------------------------------------
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

FORMAT="homebrew"
ELF=""
TITLE_ID="FAKE00001"
TITLE="EVO Player"
ICON=""

while (( $# )); do
    case "$1" in
        --format)   shift; FORMAT="${1:?--format needs a value}" ;;
        --title-id) shift; TITLE_ID="${1:?--title-id needs a value}" ;;
        --title)    shift; TITLE="${1:?--title needs a value}" ;;
        --icon)     shift; ICON="${1:?--icon needs a value}" ;;
        -h|--help)  sed -n '2,50p' "$0"; exit 0 ;;
        -*)         die "unknown option: $1 (try --help)" ;;
        *)          ELF="$1" ;;
    esac
    shift
done

[[ -n "${ELF}" ]] || die "no ELF given.
       Usage: $(basename "$0") [--format homebrew|app] <payload.elf>"

if ! in_container; then
    reexec_in_container "package-pkg.sh" --format "${FORMAT}" \
        --title-id "${TITLE_ID}" --title "${TITLE}" "${ELF}"
fi

# Resolve the ELF the same way deploy.sh does.
if [[ ! -f "${ELF}" ]]; then
    for cand in "${REPO_ROOT}/${ELF}" "${ELF_OUT}/${ELF}" "${ELF_OUT}/${ELF}.elf"; do
        [[ -f "${cand}" ]] && { ELF="${cand}"; break; }
    done
fi
need_file "${ELF}" "Build it first with ./scripts/build.sh"
validate_elf "${ELF}"

load_sdk
mkdirs

NAME="$(basename "${ELF}" .elf)"

case "${FORMAT}" in
# =============================================================================
homebrew)
    begin "building websrv homebrew bundle"

    OUT="${PKG_OUT}/${NAME}"
    rm -rf "${OUT}"
    mkdir -p "${OUT}/sce_sys"

    cp -f "${ELF}" "${OUT}/eboot.elf"

    # Icon. websrv renders sce_sys/icon0.png on its index page. Generate a
    # placeholder rather than failing - but say so, so nobody ships it.
    if [[ -n "${ICON}" ]]; then
        need_file "${ICON}"
        cp -f "${ICON}" "${OUT}/sce_sys/icon0.png"
    elif [[ -f "${REPO_ROOT}/assets/icon0.png" ]]; then
        cp -f "${REPO_ROOT}/assets/icon0.png" "${OUT}/sce_sys/icon0.png"
    else
        # A minimal valid 1x1 PNG. Replace it before release.
        printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00\nIDATx\x9cc\x00\x01\x00\x00\x05\x00\x01\r\n\x2d\xb4\x00\x00\x00\x00IEND\xaeB\x60\x82' \
            > "${OUT}/sce_sys/icon0.png"
        warn "no icon supplied - wrote a 1x1 placeholder to sce_sys/icon0.png"
        warn "provide one with --icon, or put assets/icon0.png in the repo"
    fi

    # Optional launcher metadata. websrv reads homebrew.js for custom UI.
    cat > "${OUT}/homebrew.js" <<EOF
// websrv launcher descriptor for ${TITLE}.
// Returning an empty args list simply launches eboot.elf with no arguments.
// See https://github.com/ps5-payload-dev/websrv/tree/master/homebrew/demo
function main() {
    return {
        mainText: "${TITLE}",
        secondaryText: "EVO Player - PS5 12.70 homebrew",
        args: []
    };
}
EOF

    # A zip is convenient for copying to a USB stick from Windows.
    ( cd "${PKG_OUT}" && zip -qr "${NAME}-homebrew.zip" "${NAME}" )

    echo ""
    ok "homebrew bundle: output/pkg/${NAME}/"
    ok "zipped:          output/pkg/${NAME}-homebrew.zip"
    echo ""
    echo "   Install on the console:"
    echo "     1. Extract the zip to a USB stick as:  /homebrew/${NAME}/"
    echo "     2. Plug it into the PS5, jailbreak, start ps5-payload-websrv"
    echo "     3. Open http://<ps5>:8080/index.html and launch it"
    ;;

# =============================================================================
app)
    begin "building fake-SELF application"

    MKFSELF="${PS5_PAYLOAD_SDK}/samples/install_app/make_fself.py"
    if [[ ! -f "${MKFSELF}" ]]; then
        die "make_fself.py not found at:
         ${MKFSELF}
       It ships with the SDK in samples/install_app. If your SDK install is
       incomplete, reinstall it:  ./scripts/setup-sdk.sh --from-release"
    fi
    need_cmd python3

    [[ "${TITLE_ID}" =~ ^[A-Z]{4}[0-9]{5}$ ]] \
      || die "TITLE_ID must be 4 uppercase letters + 5 digits (e.g. FAKE00001), got '${TITLE_ID}'"

    # -------------------------------------------------------------------------
    # make_fself.py only accepts a STATIC ET_EXEC ELF. An ordinary payload is
    # built as a PIE (ET_DYN) and is rejected with "Unsupported type".
    #
    # This is structural, not a flag we forgot. Look at how the SDK's
    # samples/install_app actually builds its eboot:
    #     eboot.elf: eboot.o
    #             $(LD) --static -T eboot.x -o $@ $^
    # a static link against a custom linker script - and eboot.c is a tiny
    # launcher stub, NOT the homebrew itself. The real application still runs
    # as a normal payload; the registered app just bootstraps it.
    #
    # So "convert my payload into an app" is not a thing you can do. Say so
    # plainly instead of emitting a broken eboot.bin.
    # -------------------------------------------------------------------------
    ELF_TYPE="$(llvm-readelf -h "${ELF}" 2>/dev/null | sed -n 's/ *Type: *\([A-Z]*\).*/\1/p')"
    if [[ "${ELF_TYPE}" != "EXEC" ]]; then
        die "${ELF} is a ${ELF_TYPE:-unknown} ELF; make_fself.py requires a static EXEC.

       Payloads from this repo are position-independent (ET_DYN), which the
       fake-SELF maker rejects with 'Unsupported type'. That is by design:
       an app's eboot.bin is not your payload, it is a small static launcher
       stub linked with the SDK's eboot.x linker script, which then starts the
       real payload.

       To build a home-screen app you need to port the launcher pattern from:
           \$PS5_PAYLOAD_SDK/samples/install_app/
       (eboot.c + eboot.x + payload.c). See docs/packaging.md.

       For ordinary homebrew distribution use instead:
           $(basename "$0") --format homebrew ${ELF}"
    fi

    OUT="${PKG_OUT}/${TITLE_ID}"
    rm -rf "${OUT}"
    mkdir -p "${OUT}/sce_sys"

    # These constants come straight from the SDK's samples/install_app
    # Makefile. The authinfo blob is what makes the loader accept an unsigned
    # ELF as a "fake" SELF.
    AUTHID="0x3800000000000022"
    SDK_VERSION="0x07590001"
    AUTHINFO="00 00 00 00 00 00 00 00 00 00 00 00 00 1C 00 40 00 FF 00 00 00 00 \
00 80 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 80 \
00 40 00 40 00 00 00 00 00 00 00 80 00 00 00 00 00 00 00 08 00 40 \
FF FF 00 00 00 F0 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
00 00 00 00"

    python3 "${MKFSELF}" --ptype fake --paid "${AUTHID}" \
        --auth-info "${AUTHINFO}" \
        --app-version "${SDK_VERSION}" \
        --fw-version "${SDK_VERSION}" \
        "${ELF}" "${OUT}/eboot.bin" \
      || die "make_fself.py failed to convert ${ELF}"

    # PS5 metadata is param.json - NOT the PS4's binary param.sfo.
    cat > "${OUT}/sce_sys/param.json" <<EOF
{
  "applicationCategoryType": 0,
  "titleId": "${TITLE_ID}",
  "contentId": "IV0000-${TITLE_ID}_00-EVOPLAYER0000000",
  "contentVersion": "01.00",
  "masterVersion": "01.00",
  "localizedParameters": {
    "defaultLanguage": "en-US",
    "en-US": { "titleName": "${TITLE}" }
  },
  "kernel": { "cpuPageTableSize": 0, "gpuPageTableSize": 0 },
  "pubtools": { "creationDate": "$(date -u +%Y-%m-%d)" }
}
EOF

    if [[ -n "${ICON}" ]] && [[ -f "${ICON}" ]]; then
        cp -f "${ICON}" "${OUT}/sce_sys/icon0.png"
    else
        warn "no icon supplied; add ${OUT}/sce_sys/icon0.png before installing"
    fi

    echo ""
    ok "fake-SELF app: output/pkg/${TITLE_ID}/"
    echo "     eboot.bin           $(stat -c %s "${OUT}/eboot.bin") bytes"
    echo "     sce_sys/param.json"
    echo ""
    echo "   Install it (console must be jailbroken with ftpsrv running):"
    echo "     see docs/packaging.md - the SDK's samples/install_app 'make test'"
    echo "     target shows the exact FTP + sceAppInstUtil sequence."
    ;;

# =============================================================================
pkg|fpkg)
    die "true signed fPKG creation is not implemented and cannot be.

       It requires Sony's proprietary 'prospero-pub-cmd' from the official
       Prospero SDK, which must not be committed to this repository. If you
       hold a legitimate licensed copy, place it at:
           proprietary/prospero-pub-cmd
       and see docs/packaging.md.

       For homebrew distribution you almost certainly want:
           $(basename "$0") --format homebrew ${ELF}"
    ;;

*)
    die "unknown format '${FORMAT}'. Use: homebrew | app"
    ;;
esac
