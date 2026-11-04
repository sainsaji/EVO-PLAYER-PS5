#!/bin/bash
# tools/build_frame_test.sh - build the S2b test engine host (#237) as
# output/evo_frame_test.elf. Not embedded in EVO: for the spike it is copied
# to /mnt/usb0/evo_frame_test.elf, and the broker spawns it from there.
# Runs inside the container (docker compose run --rm ps5-dev ./tools/build_frame_test.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
mkdir -p output
"${SDK}/bin/prospero-clang" -O2 -Wall -Werror -Iprojects/evoplayer/include \
    -o output/evo_frame_test.elf projects/evoplayer/helpers/frame_test.c
echo "  ok frame test payload: $(wc -c < output/evo_frame_test.elf) bytes -> output/evo_frame_test.elf"
