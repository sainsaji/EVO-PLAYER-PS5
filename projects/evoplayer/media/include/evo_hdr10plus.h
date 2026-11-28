/*
 * evo_hdr10plus.h - the per-scene peak from HDR10+ (SMPTE ST 2094-40) metadata.
 *
 * EVO cannot send HDR10+ to the TV, so it uses the scene peak itself: with the
 * HDR PEAK setting on, each scene is fitted to the panel the same way a Dolby
 * Vision shot is (map-only evo_dovi_params, see evo_dovi.h).
 */
#ifndef EVO_HDR10PLUS_H
#define EVO_HDR10PLUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scene peak in nits from an ITU-T T.35 payload that starts at the country
 * code (0xB5 ...). 0 = not HDR10+, damaged, or no usable peak. */
float evo_hdr10plus_peak_t35(const uint8_t *t35, int size);

/* The same for one HEVC Annex-B access unit: reads the prefix SEI NALs that
 * come before the first slice. 0 = this AU carries none. */
float evo_hdr10plus_peak_annexb(const uint8_t *au, int size);

#ifdef __cplusplus
}
#endif

#endif
