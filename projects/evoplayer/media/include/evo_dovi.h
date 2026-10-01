/*
 * evo_dovi - Dolby Vision Profile 5 colour for the GPU present path.
 *
 * A Profile 5 stream has no HDR10 base layer: the HEVC picture holds Dolby's
 * reshaped IPT-PQ-c2 signal, and only the per-frame RPU (HEVC NAL type 62)
 * says how to turn it back into colour. Shown as plain YCbCr it comes out
 * purple and green. Profiles 8.x carry a real HDR10/HLG base layer and play
 * correctly without any of this.
 *
 * The maths is libplacebo's (pl_shader_dovi_reshape + the DOLBYVISION branch
 * of pl_shader_decode_color): per-component reshaping curves on the raw
 * signal, the RPU's YCC->LMS' matrix, PQ EOTF, LMS->BT.2020 RGB. Output is
 * BT.2020 PQ, so from there it is an ordinary HDR10 picture.
 *
 * The RPU is parsed with FFmpeg's own parser (ff_dovi_rpu_parse), which the
 * HEVC decoder already links in.
 */
#ifndef EVO_DOVI_H
#define EVO_DOVI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The pixel shader's DoviParams uniform block, std140, uploaded verbatim.
 * Keep in step with DOVI_PARAMS in tools/gen_video_pipes.py.
 *
 *   lohi[c]   = { lowest pivot, highest pivot, has curve, - }
 *   piv       = the inner pivots of each component, two vec4 per component,
 *               padded with a 1e9 sentinel
 *   coef[c*8+i] for piece i: polynomial {c0, c1, c2, 0}, or MMR
 *               {constant, index into mmr, -, order}
 *   mmr[c*48+k] = the MMR weights, two vec4 per order
 */
#define EVO_DOVI_PIECES   8
#define EVO_DOVI_MMR_VEC4 48

typedef struct evo_dovi_params {
    float ycc[3][4];        /* YCC -> L'M'S' rows                     */
    float off[4];           /* signal offset subtracted before ycc     */
    float lms[3][4];        /* linear LMS -> linear BT.2020 RGB rows   */
    float lohi[3][4];
    float piv[6][4];
    float coef[3 * EVO_DOVI_PIECES][4];
    float mmr[3 * EVO_DOVI_MMR_VEC4][4];
} evo_dovi_params;

/* Profile number from the stream's Dolby Vision configuration record, or 0
 * when it has none. `codecpar` is an AVCodecParameters *. */
int evo_dovi_stream_profile(const void *codecpar);

/* 1 when the stream needs evo_dovi to be shown with correct colour. */
int evo_dovi_stream_needs_reshape(const void *codecpar);

/* Fill *p for a frame whose RPU has not been seen: no reshaping, the
 * reference Profile 5 matrices. */
void evo_dovi_params_default(evo_dovi_params *p);

/* AVDOVIMetadata * -> *p. Returns 0, or -1 on anything it cannot map. */
int evo_dovi_params_from_av(const void *av_dovi_metadata, evo_dovi_params *p);

/* RPU parser for one stream. */
typedef struct evo_dovi_parser evo_dovi_parser;

/* `codecpar` supplies the configuration record. NULL on failure. */
evo_dovi_parser *evo_dovi_parser_open(const void *codecpar);
void             evo_dovi_parser_close(evo_dovi_parser *p);
void             evo_dovi_parser_flush(evo_dovi_parser *p);

/*
 * Find the RPU NAL in one Annex-B HEVC access unit and parse it into *out.
 * Returns 1 when *out was filled, 0 when the AU carries no RPU, -1 when the
 * RPU did not parse.
 */
int evo_dovi_parse_annexb(evo_dovi_parser *p, const uint8_t *au, int size,
                          evo_dovi_params *out);

#ifdef __cplusplus
}
#endif

#endif /* EVO_DOVI_H */
