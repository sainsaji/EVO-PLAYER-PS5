#!/usr/bin/env bash
# =============================================================================
# tools/lucide_icon.sh - SVG -> PNG icon pipeline for Lucide icons (Issue #107)
#
# Usage:
#   ./tools/lucide_icon.sh <lucide-name> [out.png] [size]
#
# Examples:
#   ./tools/lucide_icon.sh speaker projects/evoplayer/assets/icons/icon_speaker.png
#   ./tools/lucide_icon.sh cpu projects/evoplayer/assets/icons/icon_cpu.png 96
#
# If run on a host without rsvg-convert, automatically dispatches to ps5-dev container.
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <lucide-name> [out.png] [size]" >&2
    exit 1
fi

NAME="$1"
OUT="${2:-${REPO_ROOT}/projects/evoplayer/assets/icons/icon_${NAME}.png}"
SIZE="${3:-96}"

# Ensure absolute output path
if [[ "${OUT}" != /* && "${OUT}" != ?:* ]]; then
    OUT="${REPO_ROOT}/${OUT}"
fi

# If rsvg-convert is not on PATH, re-exec inside the ps5-dev container
if ! command -v rsvg-convert >/dev/null 2>&1; then
    if command -v docker >/dev/null 2>&1; then
        # Map path into container workspace
        REL_OUT="${OUT#"${REPO_ROOT}/"}"
        docker compose run --rm ps5-dev bash -lc "./tools/lucide_icon.sh '${NAME}' '${REL_OUT}' '${SIZE}'"
        exit 0
    else
        echo "Error: rsvg-convert is not installed and Docker is not available." >&2
        exit 1
    fi
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

SVG_SRC="${TMP_DIR}/${NAME}.svg"
SVG_MOD="${TMP_DIR}/${NAME}_white.svg"

# Fetch Lucide SVG from official repo
URL="https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/${NAME}.svg"
if ! curl -sSfL "${URL}" -o "${SVG_SRC}"; then
    echo "Error: Failed to fetch Lucide icon '${NAME}' from ${URL}" >&2
    exit 1
fi

# Force stroke="#ffffff" for RmlUi white-on-transparent icon convention
sed -E 's/stroke="[^"]*"/stroke="#ffffff"/g' "${SVG_SRC}" > "${SVG_MOD}"

mkdir -p "$(dirname "${OUT}")"
rsvg-convert -w "${SIZE}" -h "${SIZE}" -o "${OUT}" "${SVG_MOD}"

echo "Generated: ${OUT} (${SIZE}x${SIZE})"
