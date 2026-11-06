#!/bin/bash
# tools/build_glslim_probe.sh - build the GLSlim probe (#237, GPU compositing
# plan) as output/glslim_probe.elf. Not embedded in EVO: it is sent to elfldr
# (:9021) with EVO closed and logs to /data/glslim_probe.log.
# Runs inside the container (docker compose run --rm ps5-dev ./tools/build_glslim_probe.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
mkdir -p output
"${SDK}/bin/prospero-clang" -O2 -Wall -Werror \
    -o output/glslim_probe.elf projects/evoplayer/helpers/glslim_probe.c -lSceGLSlimVSH -lSceSysmodule
echo "  ok glslim probe payload: $(wc -c < output/glslim_probe.elf) bytes -> output/glslim_probe.elf"
"${SDK}/bin/prospero-clang" -O2 -Wall -Werror \
    -o output/proc_diff.elf projects/evoplayer/helpers/proc_diff.c
echo "  ok proc diff payload: $(wc -c < output/proc_diff.elf) bytes -> output/proc_diff.elf"
"${SDK}/bin/prospero-clang" -O2 -Wall -Werror \
    -o output/proc_poke.elf projects/evoplayer/helpers/proc_poke.c
echo "  ok proc poke payload: $(wc -c < output/proc_poke.elf) bytes -> output/proc_poke.elf"
"${SDK}/bin/prospero-clang" -O2 -Wall -Werror \
    -o output/agc_probe.elf projects/evoplayer/helpers/agc_probe.c -lpthread -lSceSysmodule
echo "  ok agc probe payload: $(wc -c < output/agc_probe.elf) bytes -> output/agc_probe.elf"
