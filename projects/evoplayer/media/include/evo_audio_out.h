/*
 * evo_audio_out.h — media audio output path: decode → resample → AudioOut.
 *
 * Owns the AudioOut port handle, the block ring feeding it, the audio session
 * clock, the audio decode thread, and the audio-track switch. Verbatim move of
 * the audio-out / mix / AUDIO_TRACK_SWITCH regions from main.c — Track A step
 * A3 of docs/modularisation-plan.md.
 *
 * TRANSITIONAL: this module still reads a dozen playback-core globals from
 * main.c (screen, player_paused, video_clock_seconds, the subtitle/resume
 * state used by the track switch, ...) as plain externs, and main.c's
 * start_video_playback / stop_video_playback / seek still poke the audio-out
 * state exported below directly. Step A8 replaces both directions with the
 * evo_pb_*() façade (plan §4).
 */
#ifndef EVO_AUDIO_OUT_H
#define EVO_AUDIO_OUT_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_AUDIO_MAX_CH    8
#define AUDIO_QUEUE_BLOCKS  64
#define AUDIO_BLOCK_SAMPLES 2048

/* ---------------------------------------------------------------------------
 * Audio-out session state. Owned by evo_audio_out.c; still written directly by
 * main.c's start_video_playback / stop_video_playback / seek path until A8.
 * ------------------------------------------------------------------------ */
extern int                audio_handle;
extern int                audio_accum_pos;
extern volatile int       audio_queue_read;
extern volatile int       audio_queue_write;
extern volatile int       audio_queue_count;
extern volatile long long audio_samples_played;
extern volatile long long audio_samples_decoded;
extern volatile double    audio_clock_seconds;
extern volatile double    audio_pts_seconds;
extern double             first_audio_pts_seconds;
extern int                detected_audio_rate;
extern volatile int       audio_thread_running;
extern pthread_t          audio_thread;
extern volatile int       audio_decode_thread_running;
extern pthread_t          audio_decode_thread;

/* Audio-track switch: the pending stream request and the label of the live
 * track. start_video_playback reads both while selecting streams. */
extern int  prospero_audio_requested_stream;
extern char prospero_audio_active_label[128];

/* Threads (spawned by main.c via pthread_create). */
void *audio_output_thread(void *arg);
void *audio_decode_thread_func(void *arg);

/* Build the "1/3  eng  eac3  6CH"-style label for a given audio stream. */
void prospero_audio_build_label(AVFormatContext *format, int selected_stream,
                                char *output, size_t output_size);

/* Cycle to the next decodable audio track (input dispatch entry point). */
void prospero_audio_cycle_track(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_AUDIO_OUT_H */
