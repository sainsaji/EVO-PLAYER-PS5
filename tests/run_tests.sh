#!/usr/bin/env bash
# =============================================================================
# tests/run_tests.sh — Build & Execute Unit Tests with Code Coverage
# =============================================================================
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PP="${REPO_ROOT}/projects/evoplayer/pp"
UI="${REPO_ROOT}/projects/evoplayer/ui"
MEDIA="${REPO_ROOT}/projects/evoplayer/media"
ADDONS="${REPO_ROOT}/projects/evoplayer/addons"
EVO="${REPO_ROOT}/projects/evoplayer"
CORE="${REPO_ROOT}/projects/evoplayer/core"
UI_RML="${REPO_ROOT}/projects/evoplayer/ui_rml"
OUT="${REPO_ROOT}/output/tests"
COV_OUT="${REPO_ROOT}/output/coverage"

mkdir -p "${OUT}" "${COV_OUT}"
rm -f "${OUT}"/*.gcda

echo "==> Building EVO Player Test Suite with Coverage Instrumentation..."

if [ -x "/usr/bin/gcc" ]; then
    CC="/usr/bin/gcc"
elif [ -x "/usr/bin/clang" ]; then
    CC="/usr/bin/clang"
else
    CC="gcc"
fi

if [ -x "/usr/bin/g++" ]; then
    CXX="/usr/bin/g++"
elif [ -x "/usr/bin/clang++" ]; then
    CXX="/usr/bin/clang++"
else
    CXX="g++"
fi

SRCS=(
    "${REPO_ROOT}/tests/test_runner.c"
    "${REPO_ROOT}/tests/test_settings.c"
    "${CORE}/src/services/SettingsService.cpp"
    "${MEDIA}/src/evo_direct_mem.c"
    "${MEDIA}/src/evo_textreader.c"
    "${MEDIA}/src/evo_playlist_sniff.c"
    "${EVO}/src/evo_error.c"
    "${UI}/src/evo_nav.c"
    "${UI}/src/evo_focus.c"
    "${UI}/src/evo_layout.c"
    "${EVO}/src/evo_data_path.c"
    "${EVO}/src/evo_readdir.c"
    "${PP}/src/evo_theme.c"
    "${PP}/src/evo_ui.c"
    "${ADDONS}/src/addon_emby.c"
    "${ADDONS}/src/evo_net.c"
    "${ADDONS}/src/cJSON.c"
    "${PP}/src/pp_clock.c"
    "${PP}/src/pp_playback.c"
    # #90: the provider seam. Here so that -DNO_OPENSSL=1 stays a build that
    # works rather than a build nobody tries - evo_net's TLS is the only part
    # of this layer that needs OpenSSL, and everything else has to compile and
    # link without it.
    "${ADDONS}/src/evo_provider_mgr.c"
    "${ADDONS}/src/evo_provider_bundle.c"
    "${ADDONS}/src/provider_iptv.c"
    "${ADDONS}/src/provider_xtream.c"
    "${ADDONS}/src/xtream_epg.c"
    "${MEDIA}/src/evo_audio_gain.c"
    "${MEDIA}/src/evo_langpref.c"
    "${ADDONS}/src/provider_emby.c"
    "${ADDONS}/src/provider_jellyfin.c"
    "${ADDONS}/src/provider_plex.c"
    "${ADDONS}/src/provider_nuvio.c"
    "${ADDONS}/src/provider_stremio.c"
    "${ADDONS}/src/evo_online_subs.c"
    "${ADDONS}/src/evo_stremio_account.c"
    "${MEDIA}/src/evo_langname.c"
    "${EVO}/src/evo_favorites.c"
    "${EVO}/src/evo_recent.c"
    "${MEDIA}/src/evo_smb_io.c"
    "${REPO_ROOT}/tests/test_smb.cpp"
    "${CORE}/src/services/SmbClient.cpp"
    "${CORE}/src/services/FtpClient.cpp"
    "${CORE}/src/services/NetworkStorageService.cpp"
    "${REPO_ROOT}/tests/test_netart.cpp"
    "${CORE}/src/services/NetArtSidecar.cpp"
    "${CORE}/src/services/NetArtCache.cpp"
    "${REPO_ROOT}/tests/test_titles.cpp"
    "${REPO_ROOT}/tests/test_prev_run.cpp"
    "${EVO}/src/evo_prev_run.c"
    "${REPO_ROOT}/tests/test_ntfs.cpp"
    "${EVO}/src/evo_ntfs.c"
    "${CORE}/src/services/ReleaseName.cpp"
)

# Version from the same file package-app.sh and release.yml use, rather than a
# fourth hardcoded copy - this said 0.8.0-dev while the app itself said 0.7.6.
EVO_VER="$(tr -d '[:space:]' < "${EVO}/VERSION")"

# EVO_TEST_SANITIZE=1: AddressSanitizer + UBSan, for a crash that only shows on
# big inputs (out-of-bounds, use-after-free). Slower; off by default.
SAN_FLAGS=""
if [[ "${EVO_TEST_SANITIZE:-}" == "1" ]]; then
    SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
fi

OBJS=()
for src in "${SRCS[@]}"; do
    fname="$(basename "${src}")"
    obj="${OUT}/${fname%.*}.o"
    if [[ "${src}" == *.cpp || "${src}" == *"test_settings.c" ]]; then
        $CXX -O0 -g -coverage ${SAN_FLAGS} -Wall -Wextra -std=c++17 -Wno-format-truncation \
            -DNO_OPENSSL=1 \
            -DEVO_PLAYER_VERSION="\"${EVO_VER}\"" \
            -DEVO_THEME_DIR='"/tmp/evo_themes"' \
            -I"${PP}/include" \
            -I"${UI}/include" \
            -I"${UI_RML}/include" \
            -I"${MEDIA}/include" \
            -I"${ADDONS}/include" \
            -I"${EVO}/include" \
            -I"${EVO}" \
            -I"${CORE}/include" \
            -I/opt/ps5-payload-sdk/target/user/homebrew/include \
            -c "${src}" -o "${obj}"
    else
        $CC -O0 -g -coverage ${SAN_FLAGS} -Wall -Wextra -std=gnu11 -Wno-format-truncation \
            -DNO_OPENSSL=1 \
            -DEVO_PLAYER_VERSION="\"${EVO_VER}\"" \
            -DEVO_THEME_DIR='"/tmp/evo_themes"' \
            -I"${PP}/include" \
            -I"${UI}/include" \
            -I"${MEDIA}/include" \
            -I"${ADDONS}/include" \
            -I"${EVO}/include" \
            -I"${EVO}" \
            -c "${src}" -o "${obj}"
    fi
    OBJS+=("${obj}")
done

$CXX -coverage ${SAN_FLAGS} "${OBJS[@]}" -pthread -lm -o "${OUT}/test_runner"

echo "==> Running Test Suite..."
"${OUT}/test_runner"

echo "==> Generating Code Coverage Summary..."
cd "${OUT}"
gcov -b *.o > "${COV_OUT}/gcov_summary.txt" 2>&1 || true
cp -f *.gcov "${COV_OUT}/" 2>/dev/null || true
cd "${REPO_ROOT}"

if [ -f "${COV_OUT}/gcov_summary.txt" ]; then
    cat "${COV_OUT}/gcov_summary.txt"
fi

if command -v lcov &>/dev/null; then
    lcov --capture --directory "${OUT}" --output-file "${COV_OUT}/coverage.info" --rc lcov_branch_coverage=1 || true
fi

if [ -f "${REPO_ROOT}/scripts/gen-sonar-commands.py" ]; then
    python3 "${REPO_ROOT}/scripts/gen-sonar-commands.py"
fi

echo "==> All Tests and Coverage Generated Successfully!"
