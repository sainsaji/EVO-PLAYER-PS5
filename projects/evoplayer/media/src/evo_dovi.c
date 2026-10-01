/*
 * evo_dovi - Dolby Vision Profile 5 colour for the GPU present path.
 * See evo_dovi.h for what and why.
 */
#include "evo_dovi.h"

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavcodec/version.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/mem.h>

/*
 * FFmpeg's RPU parser is internal (libavcodec/dovi_rpu.h), and the source
 * tree that header lives in is not part of this repo. The parser takes a
 * caller-owned DOVIContext, so its layout has to match exactly: this is the
 * FFmpeg 7.1.1 declaration, and the build fails on any other libavcodec
 * rather than let ff_dovi_rpu_parse write through a stale struct.
 */
#if LIBAVCODEC_VERSION_MAJOR != 61 || LIBAVCODEC_VERSION_MINOR != 19
#error "evo_dovi.c mirrors FFmpeg 7.1.1's DOVIContext - recheck it against libavcodec/dovi_rpu.h"
#endif

#define DOVI_MAX_DM_ID 15

typedef struct DOVIExt DOVIExt;

typedef struct DOVIContext {
    void *logctx;
    int enable;
    AVDOVIDecoderConfigurationRecord cfg;
    AVDOVIRpuDataHeader header;
    const AVDOVIDataMapping *mapping;
    const AVDOVIColorMetadata *color;
    DOVIExt *ext_blocks;
    AVDOVIColorMetadata *dm;
    AVDOVIDataMapping *vdr[DOVI_MAX_DM_ID + 1];
    uint8_t *rpu_buf;
    unsigned rpu_buf_sz;
} DOVIContext;

extern void ff_dovi_ctx_unref(DOVIContext *s);
extern void ff_dovi_ctx_flush(DOVIContext *s);
extern int  ff_dovi_rpu_parse(DOVIContext *s, const uint8_t *rpu, size_t rpu_size,
                              int err_recognition);
extern int  ff_dovi_get_metadata(DOVIContext *s, AVDOVIMetadata **out_metadata);
extern const AVDOVIColorMetadata ff_dovi_color_default;

#define HEVC_NAL_UNSPEC62 62

struct evo_dovi_parser {
    DOVIContext ctx;
    uint8_t    *nal;        /* the RPU with emulation prevention removed */
    size_t      nal_cap;
};

static const AVDOVIDecoderConfigurationRecord *conf_record(const void *codecpar)
{
    const AVCodecParameters *par = (const AVCodecParameters *)codecpar;
    if (!par)
        return NULL;
    const AVPacketSideData *sd = av_packet_side_data_get(par->coded_side_data,
                                                         par->nb_coded_side_data,
                                                         AV_PKT_DATA_DOVI_CONF);
    if (!sd || sd->size < sizeof(AVDOVIDecoderConfigurationRecord))
        return NULL;
    return (const AVDOVIDecoderConfigurationRecord *)sd->data;
}

int evo_dovi_stream_profile(const void *codecpar)
{
    const AVDOVIDecoderConfigurationRecord *c = conf_record(codecpar);
    return c ? c->dv_profile : 0;
}

int evo_dovi_stream_needs_reshape(const void *codecpar)
{
    const AVDOVIDecoderConfigurationRecord *c = conf_record(codecpar);
    /* Profile 5 is the one without a displayable base layer
     * (bl_signal_compatibility_id 0). 8.x has HDR10/SDR/HLG underneath. */
    return c && c->dv_profile == 5 && c->rpu_present_flag && c->bl_present_flag;
}

/* --- metadata -> shader constants ---------------------------------------- */

/* Dolby Vision always ends in BT.2020-referred HPE LMS; this takes it to
 * linear BT.2020 RGB (libplacebo's dovi_lms2rgb). */
static const double k_lms2rgb[3][3] = {
    {  3.06441879, -2.16597676,  0.10155818 },
    { -0.65612108,  1.78554118, -0.12943749 },
    {  0.01736321, -0.04725154,  1.03004253 },
};

static double q2d(AVRational q) { return q.den ? (double)q.num / (double)q.den : 0.0; }

static void set_color(evo_dovi_params *p, const AVDOVIColorMetadata *c)
{
    double rgb2lms[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            p->ycc[i][j] = (float)q2d(c->ycc_to_rgb_matrix[i * 3 + j]);
            rgb2lms[i][j] = q2d(c->rgb_to_lms_matrix[i * 3 + j]);
        }
        p->ycc[i][3] = 0.0f;
        p->off[i] = (float)q2d(c->ycc_to_rgb_offset[i]);
    }
    p->off[3] = 0.0f;
    /* lms = k_lms2rgb * rgb_to_lms, as libplacebo composes them */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            double s = 0.0;
            for (int k = 0; k < 3; k++)
                s += k_lms2rgb[i][k] * rgb2lms[k][j];
            p->lms[i][j] = (float)s;
        }
        p->lms[i][3] = 0.0f;
    }
}

static void set_identity_curves(evo_dovi_params *p)
{
    for (int c = 0; c < 3; c++) {
        p->lohi[c][0] = 0.0f;
        p->lohi[c][1] = 1.0f;
        p->lohi[c][2] = 0.0f;   /* no curve: the shader passes the signal through */
        p->lohi[c][3] = 0.0f;
    }
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 4; k++)
            p->piv[i][k] = 1e9f;
}

void evo_dovi_params_default(evo_dovi_params *p)
{
    memset(p, 0, sizeof *p);
    set_identity_curves(p);
    set_color(p, &ff_dovi_color_default);
    /* FFmpeg's default offsets are not in the 0..1 signal domain the shader
     * works in; Profile 5 RPUs in the wild carry {0, 0.5, 0.5}. */
    p->off[0] = 0.0f;
    p->off[1] = 0.5f;
    p->off[2] = 0.5f;
}

int evo_dovi_params_from_av(const void *av_dovi_metadata, evo_dovi_params *p)
{
    const AVDOVIMetadata *md = (const AVDOVIMetadata *)av_dovi_metadata;
    if (!md || !p)
        return -1;
    const AVDOVIRpuDataHeader *hdr = av_dovi_get_header(md);
    const AVDOVIDataMapping   *map = av_dovi_get_mapping(md);
    const AVDOVIColorMetadata *col = av_dovi_get_color(md);
    if (hdr->bl_bit_depth < 8 || hdr->bl_bit_depth > 16 ||
        hdr->coef_log2_denom > 32)
        return -1;

    memset(p, 0, sizeof *p);
    set_identity_curves(p);
    set_color(p, col);

    const double piv_scale  = 1.0 / (double)((1u << hdr->bl_bit_depth) - 1u);
    const double coef_scale = 1.0 / (double)(1ull << hdr->coef_log2_denom);

    for (int c = 0; c < 3; c++) {
        const AVDOVIReshapingCurve *cv = &map->curves[c];
        const int np = cv->num_pivots;
        if (np < 2 || np > EVO_DOVI_PIECES + 1)
            continue;                       /* leave this component unshaped */

        p->lohi[c][0] = (float)(piv_scale * cv->pivots[0]);
        p->lohi[c][1] = (float)(piv_scale * cv->pivots[np - 1]);
        p->lohi[c][2] = 1.0f;

        /* inner pivots pick the piece; the ends only clamp */
        for (int i = 1; i < np - 1; i++)
            p->piv[c * 2 + (i - 1) / 4][(i - 1) % 4] = (float)(piv_scale * cv->pivots[i]);

        int mmr_idx = 0;
        for (int i = 0; i < np - 1; i++) {
            float *co = p->coef[c * EVO_DOVI_PIECES + i];
            if (cv->mapping_idc[i] == AV_DOVI_MAPPING_POLYNOMIAL) {
                for (int k = 0; k < 3; k++)
                    co[k] = (k <= cv->poly_order[i]) ? (float)(coef_scale * cv->poly_coef[i][k])
                                                     : 0.0f;
                co[3] = 0.0f;
            } else if (cv->mapping_idc[i] == AV_DOVI_MAPPING_MMR) {
                const int order = cv->mmr_order[i];
                if (order < 1 || order > 3 || mmr_idx + 2 * order > EVO_DOVI_MMR_VEC4)
                    return -1;
                co[0] = (float)(coef_scale * cv->mmr_constant[i]);
                co[1] = (float)mmr_idx;
                co[2] = 0.0f;
                co[3] = (float)order;
                for (int j = 0; j < order; j++) {
                    float *a = p->mmr[c * EVO_DOVI_MMR_VEC4 + mmr_idx];
                    float *b = p->mmr[c * EVO_DOVI_MMR_VEC4 + mmr_idx + 1];
                    a[0] = (float)(coef_scale * cv->mmr_coef[i][j][0]);
                    a[1] = (float)(coef_scale * cv->mmr_coef[i][j][1]);
                    a[2] = (float)(coef_scale * cv->mmr_coef[i][j][2]);
                    a[3] = 0.0f;
                    b[0] = (float)(coef_scale * cv->mmr_coef[i][j][3]);
                    b[1] = (float)(coef_scale * cv->mmr_coef[i][j][4]);
                    b[2] = (float)(coef_scale * cv->mmr_coef[i][j][5]);
                    b[3] = (float)(coef_scale * cv->mmr_coef[i][j][6]);
                    mmr_idx += 2;
                }
            } else {
                return -1;
            }
        }
    }
    return 0;
}

/* --- RPU parser ---------------------------------------------------------- */

evo_dovi_parser *evo_dovi_parser_open(const void *codecpar)
{
    evo_dovi_parser *p = (evo_dovi_parser *)calloc(1, sizeof *p);
    if (!p)
        return NULL;
    const AVDOVIDecoderConfigurationRecord *c = conf_record(codecpar);
    if (c)
        p->ctx.cfg = *c;
    p->ctx.enable = 1;
    return p;
}

void evo_dovi_parser_close(evo_dovi_parser *p)
{
    if (!p)
        return;
    ff_dovi_ctx_unref(&p->ctx);
    free(p->nal);
    free(p);
}

void evo_dovi_parser_flush(evo_dovi_parser *p)
{
    if (p)
        ff_dovi_ctx_flush(&p->ctx);
}

/* Offset of the next 00 00 01 start code at or after `i`, or `size`. */
static int next_start(const uint8_t *b, int size, int i)
{
    for (; i + 2 < size; i++)
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
            return i;
    return size;
}

int evo_dovi_parse_annexb(evo_dovi_parser *p, const uint8_t *au, int size,
                          evo_dovi_params *out)
{
    if (!p || !au || size < 6 || !out)
        return 0;

    int i = next_start(au, size, 0);
    while (i < size) {
        const int nal = i + 3;
        const int end = next_start(au, size, nal);
        if (nal + 2 < end && ((au[nal] >> 1) & 0x3f) == HEVC_NAL_UNSPEC62) {
            /* ff_dovi_rpu_parse wants the payload after the 2-byte NAL
             * header, emulation prevention bytes removed. Trailing zeroes
             * before the next start code it strips itself. */
            const int len = end - (nal + 2);
            if ((size_t)len + AV_INPUT_BUFFER_PADDING_SIZE > p->nal_cap) {
                size_t cap = (size_t)len + AV_INPUT_BUFFER_PADDING_SIZE;
                uint8_t *g = (uint8_t *)realloc(p->nal, cap);
                if (!g)
                    return -1;
                p->nal = g;
                p->nal_cap = cap;
            }
            const uint8_t *src = au + nal + 2;
            size_t n = 0;
            int zeros = 0;
            for (int k = 0; k < len; k++) {
                if (zeros >= 2 && src[k] == 3) {
                    zeros = 0;
                    continue;
                }
                zeros = src[k] ? 0 : zeros + 1;
                p->nal[n++] = src[k];
            }
            memset(p->nal + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);

            if (ff_dovi_rpu_parse(&p->ctx, p->nal, n, 0) < 0)
                return -1;
            AVDOVIMetadata *md = NULL;
            if (ff_dovi_get_metadata(&p->ctx, &md) < 0 || !md)
                return -1;
            int rc = evo_dovi_params_from_av(md, out);
            av_free(md);
            return rc == 0 ? 1 : -1;
        }
        i = end;
    }
    return 0;
}
