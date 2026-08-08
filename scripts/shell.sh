#!/usr/bin/env bash
# =============================================================================
# scripts/shell.sh - open an interactive shell in the dev container.
#
# Run from Windows (Git Bash / WSL) or from Linux/macOS:
#     ./scripts/shell.sh
#     ./scripts/shell.sh -c "clang --version"
#
# From PowerShell, either use Git Bash:
#     bash ./scripts/shell.sh
# or call compose directly:
#     docker compose run --rm ps5-dev bash -l
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

if in_container; then
    warn "already inside the dev container - starting a nested login shell."
    exec /bin/bash -l "$@"
fi

need_cmd docker

cd "${REPO_ROOT}" || die "cannot enter repository root ${REPO_ROOT}"

if ! docker info >/dev/null 2>&1; then
    die "cannot talk to the Docker daemon.
       On Windows: start Docker Desktop and wait for the whale icon to settle."
fi

# Warn (don't fail) when the image has not been built yet - compose will build.
if ! docker image inspect evo-player/ps5-dev:llvm18-sdk-v0.42 >/dev/null 2>&1; then
    warn "dev image not found; 'docker compose run' will build it now."
    warn "this takes a while on first run (SDK + prebuilt FFmpeg/SDL2 sysroot)."
fi

log "entering dev container (PS5_HOST=${PS5_HOST:-<unset>} PS5_PORT=${PS5_PORT})"

if (( $# )); then
    exec docker compose run --rm \
        -e "PS5_HOST=${PS5_HOST:-}" -e "PS5_PORT=${PS5_PORT}" \
        ps5-dev /bin/bash -l "$@"
else
    exec docker compose run --rm \
        -e "PS5_HOST=${PS5_HOST:-}" -e "PS5_PORT=${PS5_PORT}" \
        ps5-dev /bin/bash -l
fi
