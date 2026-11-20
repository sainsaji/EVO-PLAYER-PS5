/*
 * evo_dovi_fel.h - Dolby Vision Profile 7 enhancement layer: the parts that
 * need no decoder and no GPU.
 *
 * A Profile 7 stream is three things in one HEVC track: the HDR10 base layer
 * (BL), a second HEVC stream wrapped in NAL type 63 (the enhancement layer,
 * EL), and the RPU in NAL type 62. With a "minimal" EL (MEL) the EL carries
 * nothing and BL + RPU is the whole picture. With a "full" EL (FEL) it holds a
 * residual that lifts the 10-bit BL to the 12-bit master:
 *
 *     out = clamp(predict(BL) + nlq(EL), 0, 1)
 *
 * predict() is the RPU's reshaping curves (EVO already runs them for Profile
 * 5); nlq() is the non-linear inverse quantisation below, ETSI GS CCM 001's
 * linear dead zone.
 *
 * EVO does not apply the EL yet: that needs a second HEVC decode in step with
 * the base layer and a shader stage, and both need a console. This file is the
 * maths and the bitstream split, host-tested, so that work starts from known
 * numbers. Today it only tells MEL from FEL for the log.
 */
#ifndef EVO_DOVI_FEL_H
#define EVO_DOVI_FEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct evo_dovi_nlq {
    int   fel;             /* the EL changes the picture (some slope or threshold is not 0) */
    int   el_bit_depth;
    float offset[3];       /* EL code value that means "no residual", per Y, Cb, Cr */
    float slope[3];        /* the rest: units of the full signal range (0..1) */
    float threshold[3];
    float max[3];
} evo_dovi_nlq;

/* One component from the RPU's fixed-point fields (each is value << denom).
 * Recomputes n->fel; start from a zeroed struct. */
void evo_dovi_nlq_set(evo_dovi_nlq *n, int c, unsigned offset, uint64_t in_max,
                      uint64_t slope, uint64_t threshold, int coef_log2_denom);

/* The residual one EL sample adds, in units of the full signal range.
 * el_code is the sample as an integer code of el_bit_depth bits. */
float evo_dovi_nlq_residual(const evo_dovi_nlq *n, int c, float el_code);

/* Copy the enhancement layer out of an Annex-B access unit: every NAL of type
 * 63, without its 2-byte wrapper, behind a 4-byte start code - an Annex-B
 * access unit a plain HEVC decoder takes. Returns the bytes written, 0 when
 * the AU has no EL, -1 when `cap` is too small. */
int evo_dovi_el_extract(const uint8_t *au, int size, uint8_t *out, int cap);

#ifdef __cplusplus
}
#endif

#endif
