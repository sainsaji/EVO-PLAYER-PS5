/*
 * evo_hdr10plus.c - see evo_hdr10plus.h. Syntax: SMPTE ST 2094-40 Annex,
 * the same fields FFmpeg's av_dynamic_hdr_plus_from_t35 reads.
 */
#include "evo_hdr10plus.h"

#include <stddef.h>

typedef struct { const uint8_t *p; int bits, pos; } bitrd;

static int left(const bitrd *b) { return b->bits - b->pos; }

static uint32_t rd(bitrd *b, int n)   /* n <= 27; reads 0 past the end */
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++, b->pos++)
        v = (v << 1) | (b->pos < b->bits ? (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u : 0u);
    return v;
}

float evo_hdr10plus_peak_t35(const uint8_t *t35, int size)
{
    /* country 0xB5 (USA), provider 0x003C (Samsung), oriented code 0x0001,
     * application identifier 4 */
    if (!t35 || size < 7 || t35[0] != 0xB5 || t35[1] != 0x00 || t35[2] != 0x3C ||
        t35[3] != 0x00 || t35[4] != 0x01 || t35[5] != 4)
        return 0.0f;
    bitrd b = { t35 + 6, (size - 6) * 8, 0 };
    rd(&b, 8);                                  /* application_version */
    const int windows = (int)rd(&b, 2);
    if (windows < 1 || windows > 3)
        return 0.0f;
    for (int w = 1; w < windows; w++) {         /* extra windows: geometry only */
        for (int i = 0; i < 6; i++) rd(&b, 16);
        rd(&b, 8);
        for (int i = 0; i < 3; i++) rd(&b, 16);
        rd(&b, 1);
    }
    rd(&b, 27);                                 /* targeted display maximum luminance */
    if (rd(&b, 1)) {                            /* targeted display actual peak table */
        const int cells = (int)rd(&b, 5) * (int)rd(&b, 5);
        for (int i = 0; i < cells; i++) rd(&b, 4);
    }
    /* window 0 is the whole picture */
    uint32_t maxscl = 0, pct = 0;
    for (int i = 0; i < 3; i++) {
        const uint32_t v = rd(&b, 17);
        if (v > maxscl) maxscl = v;
    }
    rd(&b, 17);                                 /* average_maxrgb */
    const int n = (int)rd(&b, 4);
    for (int i = 0; i < n; i++) {
        const uint32_t percentage = rd(&b, 7), v = rd(&b, 17);
        /* the top percentiles (99, 99.98 %) stand in when maxscl is left 0 */
        if (percentage >= 95 && v > pct) pct = v;
    }
    if (left(&b) < 10)                          /* fraction_bright_pixels must still fit */
        return 0.0f;
    const uint32_t v = maxscl ? maxscl : pct;   /* 100000 = 10000 nits */
    if (v < 10 || v > 100000)
        return 0.0f;
    return (float)v / 10.0f;
}

float evo_hdr10plus_peak_annexb(const uint8_t *au, int size)
{
    if (!au || size < 6)
        return 0.0f;
    int i = 0;
    while (i + 3 < size) {
        if (!(au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)) { i++; continue; }
        const int nal = i + 3;
        if (nal + 2 > size)
            break;
        const int type = (au[nal] >> 1) & 0x3F;
        if (type < 32)                          /* first slice: prefix SEIs are behind us */
            break;
        i = nal + 2;
        if (type != 39)                         /* PREFIX_SEI */
            continue;
        /* un-escape this NAL (00 00 03 -> 00 00) up to the next start code */
        uint8_t rb[512];
        int n = 0, zeros = 0, j = i;
        for (; j < size && n < (int)sizeof rb; j++) {
            if (j + 2 < size && au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1)
                break;
            if (zeros >= 2 && au[j] == 3) { zeros = 0; continue; }
            zeros = au[j] == 0 ? zeros + 1 : 0;
            rb[n++] = au[j];
        }
        /* walk the SEI messages */
        int k = 0;
        while (k + 2 <= n) {
            int ptype = 0, psize = 0;
            while (k < n && rb[k] == 0xFF) { ptype += 255; k++; }
            if (k >= n) break;
            ptype += rb[k++];
            while (k < n && rb[k] == 0xFF) { psize += 255; k++; }
            if (k >= n) break;
            psize += rb[k++];
            if (psize > n - k)
                psize = n - k;                  /* longer than our buffer: read what is here */
            if (ptype == 4) {                   /* user_data_registered_itu_t_t35 */
                const float peak = evo_hdr10plus_peak_t35(rb + k, psize);
                if (peak > 0.0f)
                    return peak;
            }
            k += psize;
        }
    }
    return 0.0f;
}
