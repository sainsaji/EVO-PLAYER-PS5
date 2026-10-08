/*
 * evo_pt.h - PS5 HDMI Audio Bitstream Passthrough
 *
 * Allows bitstream passthrough of Dolby Digital (AC-3), Dolby Digital Plus
 * (E-AC-3, including Atmos), DTS, and AAC directly over HDMI to AV receivers
 * and soundbars using libSceAudioOut's exclusive bitstream mode.
 *
 * Sequence:
 *   1. h = sceAudioOutExOpen(0xFF, mode);
 *   2. sceAudioOutExConfigureOutput(0, 0, mode, 1, 0);
 *   3. Feed IEC 61937 bursts via sceAudioOutOutput(h, grain);
 *   4. Drain: sceAudioOutOutput(h, NULL);
 *   5. Close: sceAudioOutExClose(h);
 *   6. Restore: sceAudioOutExConfigureOutput(0, 0, 0xFF, 0xFF, 0);
 */

#ifndef EVO_PT_H
#define EVO_PT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test if HDMI sink EDID and codec support bitstream passthrough for a given FFmpeg AVCodecID.
 * Returns 1 if supported, 0 otherwise. */
int evo_pt_probe_sink_support(int av_codec_id);

/* Open the bitstream port for the given codec and switch HDMI audio mode.
 * Returns port handle (>= 1) on success, or < 0 on failure. */
int evo_pt_open(int av_codec_id);

/* Close the bitstream port, drain hardware queue, and restore normal HDMI PCM mode. */
void evo_pt_close(int handle);

/* Reset/flush internal packet buffers and queued grains (on seek). */
void evo_pt_reset(void);

/* Feed an audio packet from demuxer into the passthrough pipeline.
 * Formats IEC 61937 bursts and chops them into port grains. */
int evo_pt_push_packet(const uint8_t *data, size_t size, int sample_rate, int channels);

/* Retrieve the next port grain ready for sceAudioOutOutput.
 * Returns 1 if grain available, 0 if queue empty. */
int evo_pt_pop_grain(uint8_t *out_grain, size_t grain_bytes);

/* Number of grains currently buffered. */
size_t evo_pt_queued_grains(void);

/* Check if bitstream passthrough is currently active. */
int evo_pt_is_active(void);

/* Properties of the active bitstream carrier. */
size_t evo_pt_grain_bytes(void);
int evo_pt_grain_frames(void);
int evo_pt_sample_rate(void);
const char *evo_pt_active_codec_name(void);

/* Restore HDMI PCM mode unconditionally (safe cleanup on exit or crash). */
void evo_pt_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_PT_H */
