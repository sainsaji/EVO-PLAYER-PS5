#!/bin/bash
# tools/build_mesa_bundle.sh - the Mesa GPU stack for ProsperoWeb's WebProcess
# (#237), as ONE relocatable object: output/mesa_bundle/ps5_mesa_gpu.o.
#
# Prospero Browser's prebuilt stack (broker-client RADV + Mesa EGL/GLES/Zink +
# libps5platform) is pre-linked with ld -r, pulling only what EGL needs and
# the broker client, and every symbol but these is made local:
#   mesa_eglGetProcAddress  (Mesa's eglGetProcAddress, renamed: the fork's GL
#                            stubs own the public egl*/gl* names)
#   ps5_gpu_client_*        (bind the endpoint EVO sent)
# So nothing in it can clash with WebKit's own libraries. What stays undefined
# is libc, libkernel and libc++, which the fork's link supplies.
#
#   MSYS_NO_PATHCONV=1 docker run --rm \
#     -v "D:/Projects/EVO Player:/work" -v "D:/Projects/Prospero Browser:/pb" \
#     prospero-gpu-dev:llvm18 /work/tools/build_mesa_bundle.sh
#
# Licences: Mesa/RADV MIT, broker client LGPL-2.1+, libps5platform GPL-3.0+
# (the fork is GPL-3.0). Sources: /pb/output/gpu/MANIFEST.txt.
set -euo pipefail
WORK=${WORK:-/work}
GPU=${GPU:-/pb/output/gpu}
SDK=$GPU/sdk
NM=$SDK/bin/llvm-nm
CXX=$SDK/bin/prospero-clang++
LLD=$(command -v ld.lld-18 || command -v ld.lld)
OBJCOPY=$(command -v llvm-objcopy-18 || command -v llvm-objcopy)
STRIP=$(command -v llvm-strip-18 || command -v llvm-strip)
OUT=$WORK/output/mesa_bundle
mkdir -p "$OUT"

CLIENT=$GPU/broker-client/client.a
PLATFORM=$SDK/target/lib/libps5platform.a
GLES=("$GPU"/gles/libegl.a "$GPU"/gles/libzink.a "$GPU"/gles/libzinkwinsys.a "$GPU"/gles/libgallium.a
      "$GPU"/gles/libmesa.a "$GPU"/gles/libglapi.a "$GPU"/gles/libglsl.a "$GPU"/gles/libglcpp.a)
BUILTINS="$("$CXX" --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
[[ -f $BUILTINS ]] || BUILTINS=$(ls /usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.builtins-x86_64.a 2>/dev/null || true)
for f in "$CLIENT" "$PLATFORM" "$BUILTINS" "${GLES[@]}"; do [[ -f $f ]] || { echo "missing $f" >&2; exit 2; }; done

# Mesa's weak hooks that nothing defines resolve to 0 (as in a shared build).
python3 /pb/tools/gen_gpu_client_stubs.py "$NM" "$CLIENT" "${GLES[@]}" "$SDK/target/lib" "$OUT/absent.rsp"

# libc gaps the platform layer fills (as tools/build_gpu_client.sh). Inside the
# bundle only: the names are made local below, so the fork's libc is untouched.
PSYMS=$("$NM" --defined-only "$PLATFORM" 2>/dev/null || true)
DEFS=()
for name in qsort_r mkstemps open_memstream __xuname __assert __memset_chk regcomp regexec regfree regerror \
        localtime_r newlocale freelocale strtod_l strtof_l dladdr utimensat localeconv_l strtoll_l strtoull_l \
        strtold_l snprintf_l sscanf_l asprintf_l strcoll_l strxfrm_l strftime_l wcscoll_l wcsxfrm_l btowc_l \
        wctob_l iswctype_l mbrlen_l mbrtowc_l mbsrtowcs_l mbsnrtowcs_l wcrtomb_l wcsnrtombs_l mbtowc_l \
        ___mb_cur_max_l ___runetype_l ___tolower_l ___toupper_l __runes_for_locale __cxa_thread_atexit_impl \
        arc4random arc4random_buf arc4random_uniform gmtime_r statvfs fstatvfs futimens clock_nanosleep \
        opendir fdopendir readdir rewinddir dirfd closedir nl_langinfo nl_langinfo_l posix_fallocate access \
        openat unlinkat fchmodat fstatat mkdirat renameat memfd_create strcasestr mkstemp readlink localeconv; do
    grep -q " T ps5_$name$" <<<"$PSYMS" && DEFS+=("--defsym=$name=ps5_$name")
done

"$LLD" -r -o "$OUT/mesa_r.o" -u eglGetProcAddress -u ps5_gpu_client_bind \
    --whole-archive "$CLIENT" --no-whole-archive \
    --start-group "${GLES[@]}" "$PLATFORM" "$BUILTINS" --end-group \
    "${DEFS[@]}" "@$OUT/absent.rsp"

# The platform layer's stdio wrappers come along with other members but are never
# called (no --wrap here): their __real_ names are the plain functions.
"$OBJCOPY" --redefine-sym eglGetProcAddress=mesa_eglGetProcAddress     --redefine-sym __real_fclose=fclose --redefine-sym __real_fflush=fflush "$OUT/mesa_r.o" "$OUT/mesa_n.o"
KEEP=(--keep-global-symbol=mesa_eglGetProcAddress)
for s in $("$NM" --defined-only -g "$OUT/mesa_n.o" | awk '{print $3}' | grep '^ps5_gpu_client_' | sort -u); do
    KEEP+=("--keep-global-symbol=$s")
done
"$OBJCOPY" "${KEEP[@]}" "$OUT/mesa_n.o" "$OUT/ps5_mesa_gpu.o"
"$STRIP" --strip-debug "$OUT/ps5_mesa_gpu.o"
rm -f "$OUT/mesa_r.o" "$OUT/mesa_n.o"

"$NM" -g --defined-only "$OUT/ps5_mesa_gpu.o" | awk '{print $3}' > "$OUT/exports.txt"
"$NM" -u "$OUT/ps5_mesa_gpu.o" | awk '{print $NF}' | sort -u > "$OUT/undefined.txt"
cp "$GPU/MANIFEST.txt" "$OUT/MANIFEST-gpu-stack.txt"
echo "  ok $(wc -c < "$OUT/ps5_mesa_gpu.o") bytes, $(wc -l < "$OUT/exports.txt") exports, $(wc -l < "$OUT/undefined.txt") undefined -> output/mesa_bundle"
