/*
 * evo_vdec_ffmpeg.c — the FFmpeg implementation of evo_vdec.h.
 *
 * A near-verbatim lift of the inline avcodec_* video path out of main.c
 * (Track A step A6, docs/modularisation-plan.md §5). It owns the
 * AVCodecContext and the receive AVFrame, and adapts decoded frames into the
 * FFmpeg-free pp_frame. PURE: no clocks, no sleeps, no pp_playback, no
 * globals — pacing and present stay in the play loop.
 *
 * pp_map_avframe / pp_map_yuv420p10_to_8 / pp_pack_plane_u16_to_u8 and the
 * 10-bit pack scratch moved here verbatim.
 */
#include "evo_vdec.h"

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

/* One-shot user toast used by the 10-bit fast-path notice (kept verbatim). */
void toast(const char *title, const char *msg);

struct evo_vdec {
    evo_vdec_backend backend;
    AVCodecContext  *ctx;
    AVFrame         *frame;   /* scratch for receive; planes borrowed until next call */
    AVPacket        *pkt;     /* scratch for send */
};

/* ---------------------------------------------------------------------------
 * 10-bit planar 4:2:0 -> 8-bit yuv420p fast pack. Verbatim from main.c.
 * ------------------------------------------------------------------------ */
static uint8_t *s_p10_y = NULL;
static uint8_t *s_p10_u = NULL;
static uint8_t *s_p10_v = NULL;
static int s_p10_w = 0;
static int s_p10_h = 0;
static int s_p10_pack_toasted = 0;

static void pp_p10_scratch_free(void)
{
    free(s_p10_y); s_p10_y = NULL;
    free(s_p10_u); s_p10_u = NULL;
    free(s_p10_v); s_p10_v = NULL;
    s_p10_w = 0;
    s_p10_h = 0;
    s_p10_pack_toasted = 0;
}

static int pp_p10_scratch_ensure(int w, int h)
{
    size_t ysz, csz;
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1))
        return -1;
    if (s_p10_y && s_p10_w == w && s_p10_h == h)
        return 0;
    pp_p10_scratch_free();
    ysz = (size_t)w * (size_t)h;
    csz = (size_t)(w / 2) * (size_t)(h / 2);
    s_p10_y = (uint8_t *)malloc(ysz);
    s_p10_u = (uint8_t *)malloc(csz);
    s_p10_v = (uint8_t *)malloc(csz);
    if (!s_p10_y || !s_p10_u || !s_p10_v) {
        pp_p10_scratch_free();
        return -2;
    }
    s_p10_w = w;
    s_p10_h = h;
    return 0;
}

static void pp_pack_plane_u16_to_u8(const uint8_t *src, int src_stride,
                                    uint8_t *dst, int dst_stride,
                                    int width, int height, int shift)
{
    int y, x;
    for (y = 0; y < height; y++) {
        const uint16_t *s = (const uint16_t *)(src + (size_t)y * (size_t)src_stride);
        uint8_t *d = dst + (size_t)y * (size_t)dst_stride;
        /* Unroll 8-wide — 10-bit pack was a multi-ms stall before convert */
        x = 0;
        for (; x + 8 <= width; x += 8) {
            d[x + 0] = (uint8_t)(s[x + 0] >> shift);
            d[x + 1] = (uint8_t)(s[x + 1] >> shift);
            d[x + 2] = (uint8_t)(s[x + 2] >> shift);
            d[x + 3] = (uint8_t)(s[x + 3] >> shift);
            d[x + 4] = (uint8_t)(s[x + 4] >> shift);
            d[x + 5] = (uint8_t)(s[x + 5] >> shift);
            d[x + 6] = (uint8_t)(s[x + 6] >> shift);
            d[x + 7] = (uint8_t)(s[x + 7] >> shift);
        }
        for (; x < width; x++)
            d[x] = (uint8_t)(s[x] >> shift);
    }
}

static int pp_map_yuv420p10_to_8(const AVFrame *frame, pp_frame *out, int64_t pts_us)
{
    int w = frame->width;
    int h = frame->height;
    int cw = w / 2;
    int ch = h / 2;
    const int shift = 2; /* 10-bit in low bits of uint16 */

    if (pp_p10_scratch_ensure(w, h) != 0)
        return -3;
    if (!frame->data[0] || !frame->data[1] || !frame->data[2])
        return -4;

    pp_pack_plane_u16_to_u8(frame->data[0], frame->linesize[0],
                            s_p10_y, w, w, h, shift);
    pp_pack_plane_u16_to_u8(frame->data[1], frame->linesize[1],
                            s_p10_u, cw, cw, ch, shift);
    pp_pack_plane_u16_to_u8(frame->data[2], frame->linesize[2],
                            s_p10_v, cw, cw, ch, shift);

    memset(out, 0, sizeof(*out));
    out->format = PP_FRAME_YUV420P;
    out->width = (uint32_t)w;
    out->height = (uint32_t)h;
    out->pts_us = pts_us;
    out->planes[0] = s_p10_y;
    out->planes[1] = s_p10_u;
    out->planes[2] = s_p10_v;
    out->strides[0] = w;
    out->strides[1] = cw;
    out->strides[2] = cw;

    if (!s_p10_pack_toasted) {
        toast("CONVERT", "10-bit pack -> fast path");
        s_p10_pack_toasted = 1;
    }
    return 0;
}

static int pp_map_avframe(const AVFrame *frame, pp_frame *out, int64_t pts_us)
{
    if (!frame || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->width = (uint32_t)frame->width;
    out->height = (uint32_t)frame->height;
    out->pts_us = pts_us;
    if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
        out->format = PP_FRAME_YUV420P;
        out->planes[0] = frame->data[0];
        out->planes[1] = frame->data[1];
        out->planes[2] = frame->data[2];
        out->strides[0] = frame->linesize[0];
        out->strides[1] = frame->linesize[1];
        out->strides[2] = frame->linesize[2];
        return 0;
    }
    if (frame->format == AV_PIX_FMT_NV12) {
        out->format = PP_FRAME_NV12;
        out->planes[0] = frame->data[0];
        out->planes[1] = frame->data[1];
        out->strides[0] = frame->linesize[0];
        out->strides[1] = frame->linesize[1];
        return 0;
    }
    /* 10-bit 4:2:0 planar -> pack to 8-bit, keep fast convert */
    if (frame->format == AV_PIX_FMT_YUV420P10LE ||
        frame->format == AV_PIX_FMT_YUV420P10BE) {
        return pp_map_yuv420p10_to_8(frame, out, pts_us);
    }
    return -2;
}

/* ---------------------------------------------------------------------------
 * evo_vdec interface
 * ------------------------------------------------------------------------ */

static void ffmpeg_apply_tuning(AVCodecContext *ctx, const evo_vdec_open_params *p)
{
    if (p->thread_count > 0 && p->thread_count != EVO_VDEC_KEEP)
        ctx->thread_count = p->thread_count;
    if (p->thread_type != EVO_VDEC_KEEP)
        ctx->thread_type = p->thread_type;
#ifdef AV_CODEC_FLAG2_FAST
    if (p->flag2_fast)
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
#endif
    if (p->skip_loop_filter != EVO_VDEC_KEEP)
        ctx->skip_loop_filter = (enum AVDiscard)p->skip_loop_filter;
    if (p->skip_frame != EVO_VDEC_KEEP)
        ctx->skip_frame = (enum AVDiscard)p->skip_frame;
    if (p->skip_idct != EVO_VDEC_KEEP)
        ctx->skip_idct = (enum AVDiscard)p->skip_idct;
}

evo_vdec *evo_vdec_open(const evo_vdec_open_params *p, evo_vdec_backend *chosen)
{
    if (chosen)
        *chosen = EVO_VDEC_BACKEND_FFMPEG;   /* only backend for now */
    if (!p)
        return NULL;

    const AVCodec *dec = avcodec_find_decoder((enum AVCodecID)p->codec_id);
    if (!dec)
        return NULL;

    evo_vdec *v = (evo_vdec *)calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->backend = EVO_VDEC_BACKEND_FFMPEG;

    v->ctx = avcodec_alloc_context3(dec);
    v->frame = av_frame_alloc();
    v->pkt = av_packet_alloc();
    if (!v->ctx || !v->frame || !v->pkt) {
        evo_vdec_close(v);
        return NULL;
    }

    if (p->avctx_params &&
        avcodec_parameters_to_context(v->ctx,
                                      (const AVCodecParameters *)p->avctx_params) < 0) {
        evo_vdec_close(v);
        return NULL;
    }

    ffmpeg_apply_tuning(v->ctx, p);

    /* Feed the decoder timestamps in microseconds so best_effort_timestamp
     * comes straight back as pts_us — the caller rescales stream PTS -> us. */
    v->ctx->pkt_timebase = (AVRational){ 1, 1000000 };

    if (avcodec_open2(v->ctx, dec, NULL) < 0) {
        evo_vdec_close(v);
        return NULL;
    }
    return v;
}

int evo_vdec_send(evo_vdec *v, const uint8_t *data, int size, int64_t pts_us)
{
    if (!v || !v->ctx)
        return -1;

    av_packet_unref(v->pkt);

    int flush = !(data && size > 0);
    if (!flush) {
        /* Borrow the caller's buffer (alive for this call); avcodec_send_packet
         * copies into the decoder's own storage. dts left unset — packets are in
         * decode order and B-frame pts is non-monotonic; the decoder reorders to
         * display order and fills best_effort_timestamp from the pts we give it
         * (microseconds, matching ctx->pkt_timebase). */
        v->pkt->data = (uint8_t *)data;
        v->pkt->size = size;
        v->pkt->pts  = (pts_us == INT64_MIN) ? AV_NOPTS_VALUE : pts_us;
        v->pkt->dts  = AV_NOPTS_VALUE;
    }

    int ret = avcodec_send_packet(v->ctx, flush ? NULL : v->pkt);

    /* Drop the borrowed pointer before the next unref touches the packet. */
    v->pkt->data = NULL;
    v->pkt->size = 0;

    if (ret == 0)
        return 0;
    if (ret == AVERROR(EAGAIN))
        return 1;   /* drain receive first, packet not consumed */
    return -1;
}

int evo_vdec_receive(evo_vdec *v, pp_frame *out)
{
    if (!v || !v->ctx || !out)
        return -1;

    av_frame_unref(v->frame);

    int ret = avcodec_receive_frame(v->ctx, v->frame);
    if (ret == AVERROR(EAGAIN))
        return 0;
    if (ret < 0)
        return -1;

    int64_t pts_us = INT64_MIN;
    if (v->frame->best_effort_timestamp != AV_NOPTS_VALUE)
        pts_us = v->frame->best_effort_timestamp;
    else if (v->frame->pts != AV_NOPTS_VALUE)
        pts_us = v->frame->pts;

    if (pp_map_avframe(v->frame, out, pts_us) == 0)
        return 1;

    /* Decoded, but an exotic pixel format — caller runs the swscale path. */
    return 2;
}

void evo_vdec_flush(evo_vdec *v)
{
    if (!v)
        return;
    if (v->ctx)
        avcodec_flush_buffers(v->ctx);
    if (v->frame)
        av_frame_unref(v->frame);
    if (v->pkt)
        av_packet_unref(v->pkt);
}

void evo_vdec_close(evo_vdec *v)
{
    if (!v)
        return;
    if (v->pkt)
        av_packet_free(&v->pkt);
    if (v->frame)
        av_frame_free(&v->frame);
    if (v->ctx)
        avcodec_free_context(&v->ctx);
    free(v);
    pp_p10_scratch_free();
}

evo_vdec_backend evo_vdec_active(const evo_vdec *v)
{
    return v ? v->backend : EVO_VDEC_BACKEND_FFMPEG;
}

/* ---- FFmpeg-backend accessors ---- */
int evo_vdec_ffmpeg_width(const evo_vdec *v)
{
    return (v && v->ctx) ? v->ctx->width : 0;
}
int evo_vdec_ffmpeg_height(const evo_vdec *v)
{
    return (v && v->ctx) ? v->ctx->height : 0;
}
int evo_vdec_ffmpeg_color_trc(const evo_vdec *v)
{
    return (v && v->ctx) ? (int)v->ctx->color_trc : 0;
}
int evo_vdec_ffmpeg_pix_fmt(const evo_vdec *v)
{
    return (v && v->ctx) ? (int)v->ctx->pix_fmt : -1;
}
const char *evo_vdec_ffmpeg_codec_name(const evo_vdec *v)
{
    if (v && v->ctx && v->ctx->codec && v->ctx->codec->name)
        return v->ctx->codec->name;
    return "";
}
void *evo_vdec_ffmpeg_avframe(evo_vdec *v)
{
    return (v && v->backend == EVO_VDEC_BACKEND_FFMPEG) ? v->frame : NULL;
}
