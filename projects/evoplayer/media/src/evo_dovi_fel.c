/*
 * evo_dovi_fel.c - see evo_dovi_fel.h.
 *
 * The dequantisation follows ETSI GS CCM 001 (linear dead zone). Its scale was
 * cross-checked against the integer implementation in DoViBaker (erazortt,
 * GPL-3.0), which reduces to the float form used here whatever the EL depth.
 */
#include "evo_dovi_fel.h"

#include <string.h>

#define HEVC_NAL_UNSPEC63 63

void evo_dovi_nlq_set(evo_dovi_nlq *n, int c, unsigned offset, uint64_t in_max,
                      uint64_t slope, uint64_t threshold, int coef_log2_denom)
{
    if (!n || c < 0 || c > 2 || coef_log2_denom < 0 || coef_log2_denom > 32)
        return;
    const double one = (double)(UINT64_C(1) << coef_log2_denom);
    n->offset[c]    = (float)offset;
    n->max[c]       = (float)((double)in_max / one);
    n->slope[c]     = (float)((double)slope / one);
    n->threshold[c] = (float)((double)threshold / one);
    n->fel = 0;
    for (int i = 0; i < 3; i++)
        if (n->slope[i] != 0.0f || n->threshold[i] != 0.0f)
            n->fel = 1;
}

float evo_dovi_nlq_residual(const evo_dovi_nlq *n, int c, float el_code)
{
    if (!n || c < 0 || c > 2)
        return 0.0f;
    const float d = el_code - n->offset[c];
    if (d == 0.0f)
        return 0.0f;
    const float a = d < 0.0f ? -d : d;
    float r = (a - 0.5f) * n->slope[c] + n->threshold[c];
    if (r > n->max[c])
        r = n->max[c];
    return d < 0.0f ? -r : r;
}

/* Index of the next 00 00 01 at or after i, or size. */
static int next_start(const uint8_t *b, int size, int i)
{
    for (; i + 2 < size; i++)
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
            return i;
    return size;
}

int evo_dovi_el_extract(const uint8_t *au, int size, uint8_t *out, int cap)
{
    if (!au || !out || size < 5)
        return 0;
    int n = 0;
    for (int i = next_start(au, size, 0); i < size;) {
        const int nal = i + 3;
        int end = next_start(au, size, nal);
        i = end;
        /* a 4-byte start code's leading zero belongs to the next NAL */
        while (end > nal && au[end - 1] == 0)
            end--;
        /* wrapper header, then at least the inner NAL's own 2-byte header */
        if (end - nal < 4 || ((au[nal] >> 1) & 0x3f) != HEVC_NAL_UNSPEC63)
            continue;
        const int len = end - nal - 2;
        if (n + 4 + len > cap)
            return -1;
        out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
        memcpy(out + n, au + nal + 2, (size_t)len);
        n += len;
    }
    return n;
}
