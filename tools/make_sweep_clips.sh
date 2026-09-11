#!/usr/bin/env bash
# =============================================================================
# tools/make_sweep_clips.sh — generate the #8 codec-sweep corpus.
#
# HOST tool, not a container one: the pinned ps5-dev image ships FFmpeg as
# decode libraries for EVO, with no ffmpeg binary and no encoders. This needs a
# host ffmpeg built with libx264, libx265, libaom-av1/libsvtav1, libvpx-vp9 and
# mpeg2video.
#
#   ./tools/make_sweep_clips.sh [outdir]        # default output/sweep_clips
#
# Why generate rather than download: every clip comes from ONE deterministic
# source pattern, so the sweep's colour signatures are checkable instead of
# merely comparable. Every 8-bit BT.709 SDR clip below is the same pixels
# encoded differently, so they must land on the same colour — a codec that
# disagrees has a colour bug, and that conclusion needs no baseline run. The
# 10-bit clips deliberately differ only in transfer characteristic, which is the
# exact axis #41's bug turned on (the PQ curve was applied off the decoder's
# profile when the shader needed color_trc).
#
# Two families, because the two questions want opposite source material:
#   PERF   testsrc2 — motion and gradients, real decode work, realistic bitrate
#   COLOUR smptehdbars — large flat patches of known colour, near-lossless, so a
#          16-pixel probe lands inside a patch rather than on an edge
#
# Upload with:  tools/push_sweep_clips.sh
# =============================================================================
set -euo pipefail

OUT="${1:-output/sweep_clips}"
mkdir -p "${OUT}"

command -v ffmpeg >/dev/null || { echo "no ffmpeg on PATH" >&2; exit 1; }

# Short clips: the sweep measures a 30 s window and stops, and a decoder's
# steady-state cost is visible within a couple of hundred frames. 4K is kept
# shorter still — it is the encode that costs, not the playback.
DUR_HD=12
DUR_4K=8

have() { ffmpeg -hide_banner -encoders 2>/dev/null | grep -q " $1 "; }

# AV1: prefer SVT (fast enough for 4K); fall back to libaom.
if have libsvtav1; then AV1_ENC=libsvtav1; AV1_OPTS=(-preset 8 -crf 35)
elif have libaom-av1; then AV1_ENC=libaom-av1; AV1_OPTS=(-cpu-used 8 -crf 35 -b:v 0)
else AV1_ENC=""; fi

perf_src() {  # perf_src <w> <h> <fps> <dur>
    echo "-f lavfi -i testsrc2=size=$1x$2:rate=$3:duration=$4"
}
bars_src() {  # bars_src <w> <h> <dur>
    echo "-f lavfi -i smptehdbars=size=$1x$2:rate=24:duration=$3"
}

gen() {  # gen <label> <outfile> <ffmpeg args...>
    local label="$1" out="${OUT}/$2"; shift 2
    if [[ -f "${out}" ]]; then
        echo "  skip ${label} (exists)"
        return
    fi
    # The temp name has to keep the real extension — ffmpeg picks the muxer from
    # it, and a trailing ".part" makes it give up rather than guess.
    local tmp="${out%.*}.part.${out##*.}"
    echo "  gen  ${label}"
    ffmpeg -hide_banner -loglevel error -y "$@" "${tmp}"
    mv "${tmp}" "${out}"
}

echo "--- #8 sweep corpus -> ${OUT}"

# ---------------------------------------------------------------------------
# AV1 — the headline gap. Nothing in the corpus exercised it, and it is also the
# cleanest test of the verdict column's whole reason for existing: if this
# firmware has no AV1 decoder, the row must say `no_decoder`, not "too slow".
# ---------------------------------------------------------------------------
if [[ -n "${AV1_ENC}" ]]; then
    gen "AV1 1080p"  "EVO_TEST_av1_1080p.mp4" \
        $(perf_src 1920 1080 24 ${DUR_HD}) \
        -c:v "${AV1_ENC}" "${AV1_OPTS[@]}" -pix_fmt yuv420p
    gen "AV1 4K"     "EVO_TEST_av1_4k.mp4" \
        $(perf_src 3840 2160 24 ${DUR_4K}) \
        -c:v "${AV1_ENC}" "${AV1_OPTS[@]}" -pix_fmt yuv420p
else
    echo "  !! no AV1 encoder — the biggest coverage gap stays open"
fi

# ---------------------------------------------------------------------------
# VP9 Profile 2 (10-bit). evo_vdec_native has a resident decoder slot for this
# that has never had a file to decode.
# ---------------------------------------------------------------------------
gen "VP9 Profile 2 10-bit 1080p" "EVO_TEST_vp9p2_10bit_1080p.webm" \
    $(perf_src 1920 1080 24 ${DUR_HD}) \
    -c:v libvpx-vp9 -profile:v 2 -pix_fmt yuv420p10le -b:v 6M -row-mt 1

# ---------------------------------------------------------------------------
# HEVC 10-bit. The corpus had PQ at 1080p only, so two axes were untestable:
# 4K (outside the resident 1080p 10-bit slot), and HLG (the shader picks its
# curve off color_trc — with only PQ present, picking wrong is invisible).
# ---------------------------------------------------------------------------
gen "HEVC Main10 4K PQ" "EVO_TEST_hevc10_pq_4k.mp4" \
    $(perf_src 3840 2160 24 ${DUR_4K}) \
    -c:v libx265 -profile:v main10 -pix_fmt yuv420p10le -crf 24 \
    -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
    -x265-params "colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc" \
    -tag:v hvc1

gen "HEVC Main10 1080p HLG" "EVO_TEST_hevc10_hlg_1080p.mp4" \
    $(bars_src 1920 1080 ${DUR_HD}) \
    -c:v libx265 -profile:v main10 -pix_fmt yuv420p10le -crf 16 \
    -color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc \
    -x265-params "colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc" \
    -tag:v hvc1

# ---------------------------------------------------------------------------
# H.264 High 10 — 10-bit AVC. The resident AVC decoder is High profile 8-bit,
# so this should downgrade; the row proves it downgrades rather than misdecodes.
# ---------------------------------------------------------------------------
gen "H.264 High 10 1080p" "EVO_TEST_h264_high10_1080p.mp4" \
    $(perf_src 1920 1080 24 ${DUR_HD}) \
    -c:v libx264 -profile:v high10 -pix_fmt yuv420p10le -crf 20

# ---------------------------------------------------------------------------
# MPEG-2 — legacy, and cheap to include.
# ---------------------------------------------------------------------------
gen "MPEG-2 1080p" "EVO_TEST_mpeg2_1080p.mp4" \
    $(perf_src 1920 1080 25 ${DUR_HD}) \
    -c:v mpeg2video -pix_fmt yuv420p -b:v 20M

# ---------------------------------------------------------------------------
# 4K60 — the only clip here that puts real pressure on the budget. At 60 fps the
# frame period halves to 16.7 ms, which is the one way a `slow_decode` verdict
# can be made to fire on a path that passes comfortably at 24 fps.
# ---------------------------------------------------------------------------
gen "HEVC 8-bit 4K60" "EVO_TEST_hevc8_4k60.mp4" \
    $(perf_src 3840 2160 60 ${DUR_4K}) \
    -c:v libx265 -pix_fmt yuv420p -crf 24 -tag:v hvc1

gen "H.264 4K60" "EVO_TEST_h264_4k60.mp4" \
    $(perf_src 3840 2160 60 ${DUR_4K}) \
    -c:v libx264 -pix_fmt yuv420p -crf 23

# ---------------------------------------------------------------------------
# The colour set: ONE pattern, five codecs, near-lossless, all tagged BT.709
# SDR. These exist to be compared with each other. Every one of them is the same
# picture, so the sweep's colour signatures must agree; whichever disagrees is
# the one with the bug. No baseline run needed to draw that conclusion.
# ---------------------------------------------------------------------------
COLOUR_TAG=(-color_primaries bt709 -color_trc bt709 -colorspace bt709)

gen "COLOUR h264 1080p"  "EVO_TEST_colour_h264_1080p.mp4" \
    $(bars_src 1920 1080 ${DUR_HD}) \
    -c:v libx264 -pix_fmt yuv420p -crf 12 "${COLOUR_TAG[@]}"

gen "COLOUR hevc 1080p"  "EVO_TEST_colour_hevc_1080p.mp4" \
    $(bars_src 1920 1080 ${DUR_HD}) \
    -c:v libx265 -pix_fmt yuv420p -crf 12 "${COLOUR_TAG[@]}" -tag:v hvc1

gen "COLOUR vp9 1080p"   "EVO_TEST_colour_vp9_1080p.webm" \
    $(bars_src 1920 1080 ${DUR_HD}) \
    -c:v libvpx-vp9 -pix_fmt yuv420p -crf 10 -b:v 0 -row-mt 1 "${COLOUR_TAG[@]}"

if [[ -n "${AV1_ENC}" ]]; then
    gen "COLOUR av1 1080p" "EVO_TEST_colour_av1_1080p.mp4" \
        $(bars_src 1920 1080 ${DUR_HD}) \
        -c:v "${AV1_ENC}" -crf 20 ${AV1_ENC:+$( [[ ${AV1_ENC} == libaom-av1 ]] && echo "-b:v 0" )} \
        -pix_fmt yuv420p "${COLOUR_TAG[@]}"
fi

# Full-range (JPEG) source — validation.md lists this as "not run" for #62, and
# both the CPU reference and the shader assume limited range.
gen "COLOUR h264 1080p full-range" "EVO_TEST_colour_h264_fullrange_1080p.mp4" \
    $(bars_src 1920 1080 ${DUR_HD}) \
    -c:v libx264 -pix_fmt yuvj420p -crf 12 -color_range pc "${COLOUR_TAG[@]}"

echo ""
ls -la "${OUT}"
echo ""
echo "next: tools/push_sweep_clips.sh   (uploads to /mnt/usb0)"
