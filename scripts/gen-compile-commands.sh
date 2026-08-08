#!/usr/bin/env bash
# =============================================================================
# scripts/gen-compile-commands.sh
#
# Generate compile_commands.json so clangd / VS Code IntelliSense resolve the
# PS5 cross-compiler's headers instead of the host's.
#
# It configures the CMake build with the SDK's prospero-cmake wrapper (which
# injects toolchain/prospero.cmake) and symlinks the resulting database to the
# repository root, where .clangd and c_cpp_properties.json expect it.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

if ! in_container; then
    reexec_in_container "gen-compile-commands.sh" "$@"
fi

load_sdk
need_cmd ninja

BUILD_DIR="${REPO_ROOT}/build/cmake"

begin "configuring CMake (Ninja, cross toolchain)"
# ${CMAKE} is prospero-cmake, exported by prospero.sh. Using plain `cmake`
# here would configure for the host and poison the compile database.
"${CMAKE}" \
    -S "${REPO_ROOT}" \
    -B "${BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_BUILD_TYPE=Debug \
  || die "CMake configure failed. If it complains the project must use the
       PS5 cross toolchain, \$CMAKE is not prospero-cmake - source
       \$PS5_PAYLOAD_SDK/toolchain/prospero.sh first."

need_file "${BUILD_DIR}/compile_commands.json" \
    "CMake did not emit a compile database."

begin "publishing compile_commands.json"
# A copy, not a symlink: Windows bind mounts do not always honour symlinks,
# and VS Code on the host has to be able to read this file.
cp -f "${BUILD_DIR}/compile_commands.json" "${REPO_ROOT}/compile_commands.json"
ok "compile_commands.json ($(wc -l < "${REPO_ROOT}/compile_commands.json") lines)"

begin "building via Ninja (verifies the database is real)"
"${CMAKE}" --build "${BUILD_DIR}" \
  || die "Ninja build failed - see output above."

echo ""
ok "IntelliSense database ready"
echo "   Reload the VS Code window so clangd picks it up:"
echo "     Ctrl+Shift+P -> 'clangd: Restart language server'"
