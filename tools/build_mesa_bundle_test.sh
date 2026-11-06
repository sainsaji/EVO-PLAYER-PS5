#!/bin/bash
# tools/build_mesa_bundle_test.sh - #237: build helpers/mesa_bundle_test.c the
# way the ProsperoWeb fork builds its payload (SDK v0.41.1, the fork's GL stubs
# and src/ps5/mesa/ps5_mesa.c, output/mesa_bundle/ps5_mesa_gpu.o), as
# output/evo_gpu_client.elf for EVO's GPU spike.
#
#   MSYS_NO_PATHCONV=1 docker run --rm -v "D:/Projects/EVO Player:/work" \
#     -v "<dir with ps5-payload-dev.tar.gz v0.41.1>:/sdkdl" \
#     prospero-gpu-dev:llvm18 /work/tools/build_mesa_bundle_test.sh
set -euo pipefail
WORK=${WORK:-/work}
FORK=$WORK/third_party/ProsperoWeb
OUT=$WORK/output/mesa_bundle_test
mkdir -p "$OUT"
# The image has its own SDK at /opt/ps5-payload-sdk: v0.41.1 goes elsewhere.
mkdir -p /tmp/sdk0411
tar xzf /sdkdl/sdk.tgz -C /tmp/sdk0411
SDK=/tmp/sdk0411/opt/ps5-payload-sdk
CC=$SDK/bin/prospero-clang
CXX=$SDK/bin/prospero-clang++
INC=(-I"$SDK/target/user/homebrew/include" -I"$FORK/src/ps5/mesa")
for f in "$FORK/webkit-port/stubs/egl_stub.c" "$FORK/webkit-port/stubs/glesv2_stub.c" \
         "$FORK/src/ps5/mesa/ps5_mesa.c" "$WORK/projects/evoplayer/helpers/mesa_bundle_test.c"; do
    "$CC" -O2 -w "${INC[@]}" -c "$f" -o "$OUT/$(basename "${f%.c}").o"
done
"$CXX" -o "$OUT/evo_gpu_client.elf" "$OUT"/mesa_bundle_test.o "$OUT"/ps5_mesa.o "$OUT"/egl_stub.o \
    "$OUT"/glesv2_stub.o "$WORK/output/mesa_bundle/ps5_mesa_gpu.o" -Wl,--gc-sections -Wl,--wrap=pthread_create -lSceSysmodule -lpthread
ls -la "$OUT/evo_gpu_client.elf"
cp "$OUT/evo_gpu_client.elf" "$WORK/output/evo_gpu_client_bundle.elf"
echo "  ok -> output/evo_gpu_client_bundle.elf"
