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
OUT="${REPO_ROOT}/output/tests"
COV_OUT="${REPO_ROOT}/output/coverage"

mkdir -p "${OUT}" "${COV_OUT}"

echo "==> Building EVO Player Test Suite with Coverage Instrumentation..."

CC="${CC:-gcc}"

SRCS=(
    "${REPO_ROOT}/tests/test_runner.c"
    "${PP}/src/pp_compute_pipeline.c"
    "${MEDIA}/src/evo_direct_mem.c"
    "${MEDIA}/src/evo_textreader.c"
    "${UI}/src/evo_draw.c"
    "${UI}/src/evo_nav.c"
    "${UI}/src/evo_focus.c"
    "${UI}/src/evo_chrome.c"
    "${UI}/src/evo_widgets.c"
    "${UI}/src/evo_screens.c"
    "${PP}/src/evo_theme.c"
    "${PP}/src/evo_ui.c"
    "${ADDONS}/src/addon_emby.c"
    "${ADDONS}/src/evo_net.c"
    "${ADDONS}/src/cJSON.c"
)

OBJS=()
for src in "${SRCS[@]}"; do
    obj="${OUT}/$(basename "${src}" .c).o"
    $CC -O0 -g -coverage -Wall -Wextra -std=gnu11 -Wno-format-truncation \
        -DNO_OPENSSL=1 \
        -DEVO_PLAYER_VERSION="\"0.8.0-dev\"" \
        -DEVO_THEME_DIR='"/tmp/evo_themes"' \
        -I"${PP}/include" \
        -I"${UI}/include" \
        -I"${MEDIA}/include" \
        -I"${ADDONS}/include" \
        -I"${EVO}" \
        -c "${src}" -o "${obj}"
    OBJS+=("${obj}")
done

$CC -coverage "${OBJS[@]}" -pthread -lm -o "${OUT}/test_runner"

echo "==> Running Test Suite..."
"${OUT}/test_runner"

echo "==> Generating Code Coverage Summary..."
cd "${OUT}"
gcov -b *.o > "${COV_OUT}/gcov_summary.txt" 2>&1 || true
cd "${REPO_ROOT}"

if [ -f "${COV_OUT}/gcov_summary.txt" ]; then
    cat "${COV_OUT}/gcov_summary.txt"
fi

if command -v lcov &>/dev/null; then
    lcov --capture --directory "${OUT}" --output-file "${COV_OUT}/coverage.info" --rc lcov_branch_coverage=1 || true
fi

echo "==> All Tests and Coverage Generated Successfully!"
