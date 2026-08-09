#!/usr/bin/env bash
# =============================================================================
# tools/bench.sh - build and run the converter benchmark on the host.
#
#   ./tools/bench.sh            30 iterations at 1080p, 10 at 4K
#   ./tools/bench.sh 100        more iterations, steadier numbers
#   ./tools/bench.sh --asan     run under AddressSanitizer instead of timing
#
# Host clang, host CPU - no console involved. See the header comment in
# tools/bench_converter.c for why that is the right call for this measurement.
# =============================================================================
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/common.sh"

PP="${REPO_ROOT}/projects/evoplayer/pp"
EVO="${REPO_ROOT}/projects/evoplayer"
OUT="${OUTPUT_DIR}/bench"

ITERS=30
SAN=0
TSAN=0

while (( $# )); do
    case "$1" in
        --asan) SAN=1 ;;
        --tsan) TSAN=1 ;;
        -h|--help) sed -n '3,10p' "$0"; exit 0 ;;
        *) ITERS="$1" ;;
    esac
    shift
done

need_cmd clang
mkdir -p "${OUT}"

# -fsanitize builds are for finding races and overruns in the band splitting,
# not for timing - they are several times slower and the numbers are
# meaningless. Kept separate rather than mixed into the same run.
FLAGS=(-O2 -Wall -Wextra -pthread
       -I"${PP}/include" -I"${EVO}")

if (( SAN )); then
    FLAGS=(-O1 -g -Wall -Wextra -pthread
           -fsanitize=address,undefined
           -I"${PP}/include" -I"${EVO}")
    begin "building benchmark under ASan/UBSan"
elif (( TSAN )); then
    # ASan does not find data races, and the fused converter now carries a
    # persistent worker pool with a condition variable - exactly the code
    # where a race would be invisible until it corrupted a frame on hardware.
    FLAGS=(-O1 -g -Wall -Wextra -pthread
           -fsanitize=thread
           -I"${PP}/include" -I"${EVO}")
    begin "building benchmark under ThreadSanitizer"
else
    begin "building benchmark"
fi

clang "${FLAGS[@]}" \
    "${REPO_ROOT}/tools/bench_converter.c" \
    "${PP}/src/pp_converter_fused.c" \
    "${PP}/src/pp_converter_parallel.c" \
    "${PP}/src/pp_converter.c" \
    "${PP}/src/tile_copy.c" \
    -lm -o "${OUT}/bench" \
    || die "benchmark build failed"

ok "-> output/bench/bench"
echo ""

if (( SAN )) || (( TSAN )); then
    # A handful of iterations is plenty; the point is the sanitizer's verdict,
    # and both builds are several times slower than an -O2 one.
    "${OUT}/bench" 3
else
    "${OUT}/bench" "${ITERS}"
fi
