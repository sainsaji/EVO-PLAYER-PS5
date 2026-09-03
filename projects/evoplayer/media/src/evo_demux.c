/*
 * evo_demux.c — the demux thread + in-place seek path.
 *
 * Verbatim move of the PROSPERO_TRUE_AV_SEEK region, the two PacketQueue
 * instances and the stream indices from main.c (Track A step A5 of
 * docs/modularisation-plan.md). The only edits are `static` -> external
 * linkage on what main.c still touches and the transitional extern block
 * below.
 */
#include "evo_demux.h"

#include <stdint.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>

#include "pp_playback.h"
#include "evo_packet_queue.h"
#include "evo_audio_out.h"
#include "evo_audio_resample.h"
#include "evo_subtitle.h"
#include "evo_vdec.h"
#include "pp_stage_breadcrumb.h"

/* Matches main.c: the product video backend is always on in this build. */
#ifndef PP_BACKEND_ENABLED
#define PP_BACKEND_ENABLED 1
#endif

/* ---------------------------------------------------------------------------
 * TRANSITIONAL: playback-core decode context + flags + the app playback
 * object, all still owned by main.c. Replaced by the evo_pb_*() façade and a
 * passed-in pp_playback* at A7/A8.
 * ------------------------------------------------------------------------ */
extern AVFormatContext *play_fmt;
extern AVCodecContext  *audio_ctx;
extern evo_vdec        *g_vdec;   /* owns the video codec context (A6) */

extern int      player_paused;
extern char     current_media_path[512];
extern double   media_duration_sec;
extern double   resume_base_offset_seconds;
extern long long controls_last_used_ms;

extern int      video_decode_done;
extern int      video_decode_ready;
extern int      dbg_read_fail;
extern int      dbg_video_packets;
extern double   first_video_pts_seconds;
extern double   video_clock_seconds;

extern AVPacket *video_pending_pkt;
extern AVPacket *video_video_pending_pkt;

extern int      playback_profile;
extern int      video_packet_cap;
extern int      audio_packet_cap;

extern pp_playback g_pp_pb;

long long now_ms(void);
void      toast(const char *title, const char *msg);

/* ---------------------------------------------------------------------------
 * Demux state (exported via evo_demux.h).
 * ------------------------------------------------------------------------ */
PacketQueue video_packet_queue = { .mutex = PTHREAD_MUTEX_INITIALIZER };
PacketQueue audio_packet_queue = { .mutex = PTHREAD_MUTEX_INITIALIZER };

int video_stream_index = -1;
int audio_stream_index = -1;

volatile int demux_thread_running = 0;
pthread_t    demux_thread;


static pthread_mutex_t prospero_seek_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static volatile int prospero_seek_pending = 0;
static volatile int prospero_seek_in_progress = 0;

static double prospero_seek_target_seconds = 0.0;
static int prospero_seek_restore_paused = 0;


int prospero_request_inplace_seek(
    double target_seconds,
    int restore_paused
) {
    if (
        !play_fmt ||
        (video_stream_index < 0 && audio_stream_index < 0) ||
        !current_media_path[0]
    ) {
        return 0;
    }

    if (target_seconds < 0.0) {
        target_seconds = 0.0;
    }

    if (media_duration_sec > 1.0) {
        double maximum =
            media_duration_sec - 1.0;

        if (maximum < 0.0) {
            maximum = 0.0;
        }

        if (target_seconds > maximum) {
            target_seconds = maximum;
        }
    }

    player_paused = 1;
    controls_last_used_ms = now_ms();

    pthread_mutex_lock(
        &prospero_seek_mutex
    );

    prospero_seek_target_seconds =
        target_seconds;

    prospero_seek_restore_paused =
        restore_paused;

    prospero_seek_pending = 1;

    pthread_mutex_unlock(
        &prospero_seek_mutex
    );

    return 1;
}




static int prospero_process_seek_request(void) {
    double target_seconds;
    int restore_paused;

    pthread_mutex_lock(
        &prospero_seek_mutex
    );

    if (!prospero_seek_pending) {
        pthread_mutex_unlock(
            &prospero_seek_mutex
        );

        return 0;
    }

    target_seconds =
        prospero_seek_target_seconds;

    restore_paused =
        prospero_seek_restore_paused;

    prospero_seek_pending = 0;
    prospero_seek_in_progress = 1;

    pthread_mutex_unlock(
        &prospero_seek_mutex
    );

#if PP_BACKEND_ENABLED
    pp_playback_notify_seek_begin(
        &g_pp_pb,
        (int64_t)(target_seconds * 1000000.0)
    );
#endif

    /*
     * Let decoder/output threads observe player_paused.
     */
    usleep(5000);

    
    /*
     * Clear EOF immediately. This wakes the video and audio decoder
     * loops while the seek and queue reset are being completed.
     */
    video_decode_done = 0;
    video_decode_ready = 1;
    dbg_read_fail = 0;

packet_queue_clear(
        &video_packet_queue
    );

    packet_queue_clear(
        &audio_packet_queue
    );

    if (video_pending_pkt) {
        av_packet_free(
            &video_pending_pkt
        );

        video_pending_pkt = NULL;
    }

    if (video_video_pending_pkt) {
        av_packet_free(
            &video_video_pending_pkt
        );

        video_video_pending_pkt = NULL;
    }

    audio_queue_count = 0;
    audio_queue_read = 0;
    audio_queue_write = 0;
    audio_accum_pos = 0;

    double decoder_seek_seconds =
        target_seconds;

    /*
     * Move slightly backward so inter-frame codecs can begin from
     * a nearby keyframe.
     */
    /* Video: back up slightly for keyframe. Audio-only: seek near target. */
    if (video_stream_index >= 0 && decoder_seek_seconds > 0.50) {
        decoder_seek_seconds -= 0.50;
    }

    int seek_stream =
        video_stream_index >= 0
            ? video_stream_index
            : audio_stream_index;

    AVRational time_base =
        play_fmt->streams[
            seek_stream
        ]->time_base;

    int64_t seek_timestamp =
        (int64_t)(
            decoder_seek_seconds /
            av_q2d(time_base)
        );

    int result =
        av_seek_frame(
            play_fmt,
            seek_stream,
            seek_timestamp,
            AVSEEK_FLAG_BACKWARD
        );

    /* Big files (14 GB GTA trailer) whose stream index doesn't cover the byte
     * range can fail the timestamp seek; fall back to a byte seek. */
    if (result < 0) {
        result = av_seek_frame(play_fmt, seek_stream, seek_timestamp,
                               AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    }
#if defined(EVO_VDEC_LOG)
    {
        char d[80];
        snprintf(d, sizeof d, "rc=%d ts=%lld strm=%d",
                 result, (long long)seek_timestamp, seek_stream);
        pp_stage_bc("SEEK_AVFRAME", d);   /* #32 diagnostics; --usb-remote only */
    }
#endif

    if (result >= 0) {
        avformat_flush(play_fmt);

        evo_vdec_flush(g_vdec);   /* video codec + scratch frame/packet (A6) */

        if (audio_ctx) {
            avcodec_flush_buffers(
                audio_ctx
            );
        }

        prospero_audio_resampler_reset();

        if (
            prospero_embedded_subtitle_ctx
        ) {
            avcodec_flush_buffers(
                prospero_embedded_subtitle_ctx
            );
        }

        prospero_embedded_subtitle_reset();

        /*
         * The UI position is base offset plus the new audio clock.
         */
        resume_base_offset_seconds =
            target_seconds;

        audio_samples_played = 0;
        audio_samples_decoded = 0;

        audio_clock_seconds = 0.0;
        audio_pts_seconds = 0.0;
        video_clock_seconds = 0.0;

        first_audio_pts_seconds = -1.0;
        first_video_pts_seconds = -1.0;

        video_decode_done = 0;
        dbg_read_fail = 0;

        /*
         * Older builds allowed the audio decoder thread to exit at
         * EOF. Restart it if this session reached EOF before seeking.
         */
        if (
            audio_ctx &&
            !audio_decode_thread_running
        ) {
            audio_decode_thread_running = 1;

            pthread_create(
                &audio_decode_thread,
                NULL,
                audio_decode_thread_func,
                NULL
            );
        }

    } else {
        toast(
            "SEEK",
            "Decoder seek failed"
        );
    }

    prospero_seek_in_progress = 0;

#if PP_BACKEND_ENABLED
    pp_playback_notify_seek_end(
        &g_pp_pb,
        result >= 0,
        0,
        0
    );
    /* notify_seek_begin() paused the clock. Always lift that if we were
     * playing — on a FAILED seek notify_seek_end() only clears seek_discarding
     * and leaves the clock paused, which drops every frame -> frozen picture. */
    if (!restore_paused)
        pp_playback_resume(&g_pp_pb);
#endif

    player_paused =
        restore_paused ? 1 : 0;

    controls_last_used_ms =
        now_ms();

    return result >= 0;
}


void *demux_thread_func(void *arg) {
    (void)arg;

    AVPacket *pkt =
        av_packet_alloc();

    if (!pkt) {
        return NULL;
    }

    while (demux_thread_running) {
        /*
         * Process seek requests before checking the paused state.
         */
        if (prospero_process_seek_request()) {
            av_packet_unref(pkt);
            continue;
        }

        if (player_paused) {
            usleep(1000);
            continue;
        }

        int read_result =
            av_read_frame(
                play_fmt,
                pkt
            );

        if (read_result < 0) {
            /*
             * Keep the demux thread alive so seeking backward from EOF
             * does not require reopening the file.
             */
            video_decode_done = 1;
            usleep(5000);
            continue;
        }

        video_decode_done = 0;

        if (
            pkt->stream_index ==
            video_stream_index
        ) {
            int vq = packet_queue_count(&video_packet_queue);
            /*
             * Do not drop non-keyframes in demux — that freezes for a full GOP
             * (often every 1–2s). Cap queue by waiting only.
             */
            while (
                demux_thread_running &&
                !player_paused &&
                packet_queue_count(
                    &video_packet_queue
                ) >= video_packet_cap
            ) {
                usleep(playback_profile >= 3 ? 300 : 500);
            }

            if (
                demux_thread_running &&
                !player_paused
            ) {
                packet_queue_push(
                    &video_packet_queue,
                    pkt
                );

                dbg_video_packets++;
            }
        } else if (
            pkt->stream_index ==
            audio_stream_index
        ) {
            while (
                demux_thread_running &&
                !player_paused &&
                packet_queue_count(
                    &audio_packet_queue
                ) >= audio_packet_cap
            ) {
                usleep(1000);
            }

            if (
                demux_thread_running &&
                !player_paused
            ) {
                packet_queue_push(
                    &audio_packet_queue,
                    pkt
                );
            }
        }

        if (
            pkt->stream_index ==
            prospero_embedded_subtitle_stream_index
        ) {
            dbg_sub_demuxed++;

            prospero_embedded_subtitle_decode_packet(
                pkt
            );
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return NULL;
}

