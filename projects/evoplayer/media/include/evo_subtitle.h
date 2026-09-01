/*
 * evo_subtitle.h — subtitle engine: embedded (MKV text tracks) + external SRT.
 *
 * Verbatim move of the EMBEDDED_SUBTITLE_MODULE, SRT_MODULE and
 * SUBTITLE_CONTROLS regions from main.c — Track A step A4 of
 * docs/modularisation-plan.md.
 *
 * TRANSITIONAL: still reads playback-core / resume globals from main.c and
 * the rr_text renderer as plain externs; main.c's seek + start_video_playback
 * still poke prospero_embedded_subtitle_ctx / dbg_sub_demuxed directly. A8
 * (façade) and the eventual rr_* renderer module clean both directions up.
 */
#ifndef EVO_SUBTITLE_H
#define EVO_SUBTITLE_H

#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Below this a subtitle track is signage or a watermark, not dialogue.
 * Used by the embedded-track scorer and the media-info track picker. */
#define PROSPERO_SUBTITLE_MIN_USEFUL_CUES 10

/* External-SRT cue geometry — prospero_subtitle_draw() (still in main.c)
 * consumes cues and wraps them with these bounds. */
#define PROSPERO_SUBTITLE_TEXT_SIZE  512
#define PROSPERO_SUBTITLE_MAX_LINES  3
#define PROSPERO_SUBTITLE_LINE_SIZE  160
#define PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE 768

typedef struct {
    double start_seconds;
    double end_seconds;
    char   text[PROSPERO_SUBTITLE_TEXT_SIZE];
} ProsperoSubtitleCue;

/* ---------------------------------------------------------------------------
 * Subtitle state. Owned by evo_subtitle.c; read (and, for the marked few,
 * written) across main.c's OSD, settings screens, track picker, debug overlay
 * and the demux/seek path until A8.
 * ------------------------------------------------------------------------ */
extern int prospero_subtitle_count;              /* external-SRT cue count   */
extern int prospero_subtitle_enabled;
extern int prospero_subtitle_face;               /* EVO_FACE_SUB/MENU/TITLE  */
extern int prospero_subtitle_delay_ms;
extern int prospero_subtitle_requested_stream;   /* -2 auto, -1 SRT, >=0 emb */
extern int prospero_subtitle_use_external;
extern int prospero_embedded_subtitle_stream_index;
extern int prospero_embedded_subtitle_count;

/* Written by main.c's seek / start_video_playback flush path (A5 cleans up). */
extern AVCodecContext *prospero_embedded_subtitle_ctx;

/* Subtitle pipeline counters — dbg_sub_demuxed is bumped by the demux thread
 * in main.c; all five are read by the debug overlay. */
extern int dbg_sub_demuxed;
extern int dbg_sub_entered;
extern int dbg_sub_blank;
extern int dbg_sub_added;
extern int dbg_sub_cid;

/* ---- lifecycle (demux/seek/open/close) ---- */
void prospero_subtitle_clear(void);
int  prospero_subtitle_load_for_media(const char *media_path);
int  prospero_embedded_subtitle_open(AVFormatContext *format);
void prospero_embedded_subtitle_reset(void);
void prospero_embedded_subtitle_close(void);
void prospero_embedded_subtitle_decode_packet(AVPacket *packet);

/* ---- query used by the media-info track picker ---- */
int  prospero_embedded_subtitle_supported(enum AVCodecID codec_id);
int  prospero_subtitle_declared_cues(AVStream *stream);

/* ---- render helpers for prospero_subtitle_draw() (which lives in main.c) ---- */
const ProsperoSubtitleCue *prospero_subtitle_active_cue(double position);
int  prospero_embedded_subtitle_text_at(double position, char *output,
                                        size_t output_size);
void prospero_subtitle_trim(char *text);   /* also used by wrap_text in main.c */

/* ---- controls (input dispatch + settings screen) ---- */
void prospero_subtitle_toggle(void);
void prospero_subtitle_apply_track(int track);
void prospero_subtitle_nudge_delay(int delta_ms);

#ifdef __cplusplus
}
#endif

#endif /* EVO_SUBTITLE_H */
