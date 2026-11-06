#!/bin/bash
# tools/build_gpu_client.sh - build the #237 GPU broker spike client
# (projects/evoplayer/helpers/gpu_client_spike.cpp) as output/evo_gpu_client.elf,
# a payload the browser broker spawns. It links Prospero Browser's prebuilt GPU
# stack (broker-client RADV + Mesa EGL/GLES/Zink, PS5_PayloadSDK fork with
# libps5platform), so it runs in that stack's image, not ps5-dev:
#
#   MSYS_NO_PATHCONV=1 docker run --rm \
#     -v "D:/Projects/EVO Player:/work" -v "D:/Projects/Prospero Browser:/pb" \
#     prospero-gpu-dev:llvm18 /work/tools/build_gpu_client.sh
#
# GPU stack: /pb/output/gpu (MANIFEST.txt there: PS5_Mesa 1ba9874, PS5_Vulkan
# f410e67, PS5_PayloadSDK b83202b). Mesa/RADV MIT, broker client LGPL-2.1+,
# libps5platform GPL-3.0+.
set -euo pipefail
WORK=${WORK:-/work}
GPU=${GPU:-/pb/output/gpu}
SDK=$GPU/sdk
CXX=$SDK/bin/prospero-clang++
NM=$SDK/bin/llvm-nm
OUT=$WORK/output/gpu_client
mkdir -p "$OUT"

CLIENT=$GPU/broker-client/client.a
PLATFORM=$SDK/target/lib/libps5platform.a
GLES=("$GPU"/gles/libegl.a "$GPU"/gles/libzink.a "$GPU"/gles/libzinkwinsys.a "$GPU"/gles/libgallium.a
      "$GPU"/gles/libmesa.a "$GPU"/gles/libglapi.a "$GPU"/gles/libglsl.a "$GPU"/gles/libglcpp.a)
# compiler-rt (__cpu_model for addrlib's SIMD dispatch).
BUILTINS="$("$CXX" --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
[[ -f $BUILTINS ]] || BUILTINS=$(ls /usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.builtins-x86_64.a 2>/dev/null || true)
for f in "$CLIENT" "$PLATFORM" "$BUILTINS" "${GLES[@]}"; do [[ -f $f ]] || { echo "missing $f" >&2; exit 2; }; done

"$CXX" -O2 -std=c++17 -Wall -c "$WORK/projects/evoplayer/helpers/gpu_client_spike.cpp" -o "$OUT/gpu_client_spike.o" \
    -I"$GPU/gles/include" -I"$GPU/broker-client/include" -I"$WORK/projects/evoplayer/include"

# Mesa's weak hooks that nothing defines must resolve to 0 (Prospero Browser's generator).
python3 /pb/tools/gen_gpu_client_stubs.py "$NM" "$CLIENT" "${GLES[@]}" "$SDK/target/lib" "$OUT/absent.rsp"

# The platform layer's libc gaps and 2 MiB-stack thread wrappers (PS5_Vulkan radv-link.sh).
# malloc is NOT wrapped: its heap is direct memory, and a payload gets only 4 MB of that.
PSYMS=$("$NM" --defined-only "$PLATFORM" 2>/dev/null || true)
FLAGS=(-Wl,--wrap=pthread_create -Wl,--wrap=pthread_join -Wl,--wrap=pthread_detach)
for name in qsort_r mkstemps open_memstream __xuname __assert __memset_chk regcomp regexec regfree regerror \
        localtime_r newlocale freelocale strtod_l strtof_l dladdr utimensat localeconv_l strtoll_l strtoull_l \
        strtold_l snprintf_l sscanf_l asprintf_l strcoll_l strxfrm_l strftime_l wcscoll_l wcsxfrm_l btowc_l \
        wctob_l iswctype_l mbrlen_l mbrtowc_l mbsrtowcs_l mbsnrtowcs_l wcrtomb_l wcsnrtombs_l mbtowc_l \
        ___mb_cur_max_l ___runetype_l ___tolower_l ___toupper_l __runes_for_locale __cxa_thread_atexit_impl \
        arc4random arc4random_buf arc4random_uniform gmtime_r statvfs fstatvfs futimens clock_nanosleep \
        opendir fdopendir readdir rewinddir dirfd closedir nl_langinfo nl_langinfo_l posix_fallocate access \
        openat unlinkat fchmodat fstatat mkdirat renameat memfd_create strcasestr mkstemp readlink localeconv; do
    grep -q " T ps5_$name$" <<<"$PSYMS" && FLAGS+=("-Wl,--defsym=$name=ps5_$name")
done

"$CXX" -o "$OUT/evo_gpu_client.elf" "$OUT/gpu_client_spike.o" \
    -Wl,--whole-archive "$CLIENT" -Wl,--no-whole-archive \
    -Wl,--start-group "${GLES[@]}" "$PLATFORM" "$BUILTINS" -Wl,--end-group \
    "${FLAGS[@]}" -Wl,--gc-sections "-Wl,@$OUT/absent.rsp" \
    -lSceSysmodule -lpthread 2>&1 | tail -40
"$SDK/bin/llvm-strip" --strip-debug "$OUT/evo_gpu_client.elf" 2>/dev/null || llvm-strip-18 --strip-debug "$OUT/evo_gpu_client.elf"
ls -la "$OUT/evo_gpu_client.elf"
cp "$OUT/evo_gpu_client.elf" "$WORK/output/evo_gpu_client.elf"
echo "  ok gpu client payload: $(wc -c < "$OUT/evo_gpu_client.elf") bytes -> output/evo_gpu_client.elf"
