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
#include <stdio.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>

#include "pp_playback.h"
#include "evo_packet_queue.h"
#include "evo_audio_out.h"
#include "evo_audio_resample.h"
#include "evo_subtitle.h"
#include "evo_vdec.h"
#include "evo_adec.h"
#include "pp_stage_breadcrumb.h"

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
extern volatile double resume_base_anchor_pending;
extern long long controls_last_used_ms;

extern int      video_decode_done;
extern int      video_decode_ready;
extern int      dbg_read_fail;
extern int      dbg_video_packets;
extern double   first_video_pts_seconds;
extern double   video_clock_seconds;

extern AVPacket *video_pending_pkt;
extern AVPacket *video_video_pending_pkt;
extern volatile int video_thread_running;  /* evo_playback.c */
extern volatile int video_decode_parked;
extern volatile int video_decode_hold;

extern int      playback_profile;
extern int      video_packet_cap;
extern int      audio_packet_cap;

/* Start-of-stream pre-buffer. Armed by PlaybackController for a network
 * source, cleared here - see the note in Bridge.cpp. */
extern volatile int pb_prebuffer_hold;
extern int          pb_prebuffer_packets;
extern int          pb_prebuffer_max_ms;

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

    pp_playback_notify_seek_begin(
        &g_pp_pb,
        (int64_t)(target_seconds * 1000000.0)
    );

    /*
     * Wait for the video decode thread to be out of the decoder before
     * anything below touches it: evo_vdec_flush() and the pending-packet free
     * both race a decode call still in flight. The old fixed 5 ms was enough
     * while a decode call was short; a 4K AV1 frame in dav1d is not, and a
     * .mkv seek (av_seek_frame ~0 ms) flushed dav1d under a live
     * dav1d_send_data - SIGSEGV in dav1d_parse_obus (#94, hardware
     * 2026-09-26). Bounded, so a wedged decode (#39) cannot hang the seek.
     *
     * video_decode_hold, not player_paused: a committed scrub requests the
     * seek and then moves the FSM straight to Playing, whose entry clears
     * player_paused before this thread even gets here - so the decode thread
     * never parked and every seek sat out the full 2 s timeout, then flushed
     * unsynchronized anyway. The hold belongs to the seek alone.
     */
    video_decode_hold = 1;
    usleep(5000);
    if (video_thread_running) {
        int waited_ms = 0;
        while (!video_decode_parked && waited_ms < 2000) {
            usleep(1000);
            waited_ms++;
        }
        if (!video_decode_parked || waited_ms > 50) {
            char d[64];
            snprintf(d, sizeof d, "parked=%d waited_ms=%d",
                     (int)video_decode_parked, waited_ms);
            pp_stage_bc("SEEK_PARK", d);
        }
    }

    
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
     * No extra backstep. AVSEEK_FLAG_BACKWARD already lands on the keyframe at
     * or before this timestamp, which is exactly what an inter-frame codec
     * needs to restart. The 0.5 s that used to be subtracted here only widened
     * the run-up the decoder then has to chew through and throw away - and
     * when the target sat just after a keyframe it pushed the seek back a
     * whole extra GOP, which is what made a longer seek hitch harder than a
     * short one.
     */

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

    /* #94: a seek in a raw .obu (no index - the demuxer scans forward from
     * the last keyframe it has seen) took EVO down with nothing after it in
     * evo.log. This line and the ms= on SEEK_AVFRAME bracket the call. */
    {
        char d[112];
        snprintf(d, sizeof d, "fmt=%s ts=%lld target=%.3f",
                 play_fmt->iformat ? play_fmt->iformat->name : "?",
                 (long long)seek_timestamp, target_seconds);
        pp_stage_bc("SEEK_BEGIN", d);
    }
    const int64_t seek_t0 = av_gettime_relative();

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
    {
        char d[112];
        snprintf(d, sizeof d, "rc=%d ts=%lld strm=%d target=%.3f ms=%lld",
                 result, (long long)seek_timestamp, seek_stream, target_seconds,
                 (long long)((av_gettime_relative() - seek_t0) / 1000));
        pp_stage_bc("SEEK_AVFRAME", d);   /* #32 diagnostics -> /mnt/usb0/evo.log */
    }

    if (result >= 0) {
        /*
         * av_seek_frame() already flushes the demuxer before it repositions,
         * then sets the stream's running dts to the timestamp it landed on.
         * Flushing again here resets that dts. A container stamps its own
         * packets so it never mattered - but a stream with no timestamps (raw
         * .obu, AVFMT_NOTIMESTAMPS) has only that dts, and after the extra
         * flush restarted at 0: every frame then read as before the target
         * and the whole seek was decoded and thrown away in the dark (#94,
         * Chimera: a jump to 118 s discarded 1005 frames and never played).
         */
        if (!(play_fmt->iformat->flags & AVFMT_NOTIMESTAMPS))
            avformat_flush(play_fmt);

        evo_vdec_flush(g_vdec);   /* video codec + scratch frame/packet (A6) */
        if (g_adec) {
            evo_adec_flush(g_adec);
        }

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
        resume_base_anchor_pending = -1.0;

        /*
         * Arm the audio discard window before the decode threads are let go,
         * so the run-up between the keyframe this seek landed on and the
         * target is dropped on the audio side too. Both clocks then restart
         * from the target and the picture resumes without waiting for audio.
         */
        if (target_seconds > 0.05)
            audio_seek_discard_until = target_seconds;
        else
            audio_seek_discard_until = -1.0;

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
        audio_seek_discard_until = -1.0;
        toast(
            "SEEK",
            "Decoder seek failed"
        );
    }

    prospero_seek_in_progress = 0;
    video_decode_hold = 0;

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

    player_paused =
        restore_paused ? 1 : 0;

    controls_last_used_ms =
        now_ms();

    return result >= 0;
}


/*
 * Release the pre-buffer hold once the queue has a cushion, the deadline has
 * passed, or the stream ended. Called from the demux loop after each packet.
 * `ended` is set on a read failure, where waiting for depth that will never
 * arrive would park the decode threads for the whole deadline.
 */
static void prebuffer_check(long long deadline_ms, int ended)
{
    if (!pb_prebuffer_hold)
        return;

    /* An audio-only stream never fills the video queue, so measure whichever
     * queue this stream actually feeds. */
    const int have_video = (video_stream_index >= 0);
    const int depth      = have_video ? packet_queue_count(&video_packet_queue)
                                      : packet_queue_count(&audio_packet_queue);

    /*
     * Never ask for more than the queue can hold. At or above the cap the
     * demux thread parks in the queue-full wait below and never gets back
     * here to clear the hold - the decode threads would stay parked forever.
     */
    const int cap    = have_video ? video_packet_cap : audio_packet_cap;
    int       target = pb_prebuffer_packets;
    if (target > cap - 1) target = cap - 1;
    if (target < 1)       target = 1;

    /*
     * A queue at its cap is as much cushion as this stream will ever get, so
     * release on that too rather than sitting out the deadline. Without it, a
     * stream whose audio caps out before video reaches its target would buffer
     * for the full deadline every time it is opened.
     */
    const int any_full =
        packet_queue_count(&video_packet_queue) >= video_packet_cap ||
        packet_queue_count(&audio_packet_queue) >= audio_packet_cap;

    const int timed_out = (now_ms() >= deadline_ms);
    if (!ended && !timed_out && !any_full && depth < target)
        return;

    pb_prebuffer_hold = 0;
    {
        char d[64];
        snprintf(d, sizeof d, "%s packets=%d target=%d",
                 ended     ? "ended"
                 : timed_out ? "deadline"
                 : any_full  ? "queue-full"
                             : "filled",
                 depth, target);
        pp_stage_bc("P8_03_PREBUFFER_DONE", d);
    }
}

/*
 * Low-water mark on the OTHER stream's queue. Below this, that decoder is
 * within a fraction of a second of running dry and the demux thread must not
 * stay parked on a full queue - see demux_wait_for_room().
 */
#define DEMUX_STARVE_LOW_PACKETS 12

/*
 * Hard ceiling on how far a queue may overshoot its cap while the other stream
 * is starving. The cap is a memory guard, not a correctness invariant, and
 * PACKET_QUEUE_SIZE (512) is the real limit - packet_queue_push() simply
 * returns 0 there, so overshooting can never corrupt the ring.
 */
#define DEMUX_OVERSHOOT_FACTOR 2

/*
 * Wait for room in `q`, but never at the cost of starving the other stream.
 *
 * av_read_frame() hands packets back in interleave order, so parking here on a
 * full queue also stops the OTHER stream's packets arriving. That is one leg of
 * a four-way deadlock hit on hardware after seeking into 4K HEVC + E-AC-3
 * (2026-09-28, Avatar UHD remux):
 *
 *   audio output parks because the audio clock is >0.5 s ahead of video
 *     -> the decoded-PCM ring stays above its high-water mark
 *     -> the audio decode thread stops popping packets
 *     -> the audio packet queue caps out
 *     -> the demux thread parks HERE
 *     -> the video packet queue drains to empty
 *     -> video stops decoding, so video_clock_seconds stops advancing
 *     -> audio output's "ahead of video" test is now permanently true.
 *
 * Nothing moves again. evo.log showed video_frames=0 for two minutes with
 * byte-identical allocator counters (no av_read_frame at all) while the UI
 * carried on at 59.9 fps.
 *
 * Releasing as soon as the other queue runs dry cuts that cycle: the queue in
 * hand overshoots its cap by a bounded amount instead, which costs a few MB
 * and keeps both decoders fed.
 */
static void demux_wait_for_room(PacketQueue *q, int cap,
                                PacketQueue *other, int other_cap,
                                long long prebuffer_deadline_ms, int sleep_us)
{
    int limit = cap * DEMUX_OVERSHOOT_FACTOR;
    if (limit > PACKET_QUEUE_SIZE - 1)
        limit = PACKET_QUEUE_SIZE - 1;

    /* An other-stream low-water above its own cap would release immediately
     * and turn the cap off altogether; keep it strictly below. */
    int starve_low = DEMUX_STARVE_LOW_PACKETS;
    if (other_cap > 0 && starve_low > other_cap / 2)
        starve_low = other_cap / 2;

    while (demux_thread_running &&
           !player_paused &&
           packet_queue_count(q) >= cap) {
        /* Still re-check the pre-buffer here: this loop does not return to the
         * top of the demux loop, so it is the only place the deadline can fire
         * once a queue is full. It is also what keeps the pre-buffer itself
         * from deadlocking when audio caps out before video reaches its
         * target (audio is ~43 pkt/s against 30 fps). */
        prebuffer_check(prebuffer_deadline_ms, 0);

        /* The other decoder is about to run dry and only this thread can feed
         * it. Overshoot rather than deadlock. */
        if (other && packet_queue_count(other) < starve_low &&
            packet_queue_count(q) < limit) {
            /* Rate-limited: this fires per packet once it starts, and one
             * line per 2 s is enough to tell a starved interleave apart from
             * a healthy one in evo.log without flooding it. */
            static long long s_last_bc_ms = 0;
            long long now = now_ms();
            if (now - s_last_bc_ms >= 2000) {
                char d[80];
                snprintf(d, sizeof d, "q=%d cap=%d limit=%d other=%d low=%d",
                         packet_queue_count(q), cap, limit,
                         packet_queue_count(other), starve_low);
                pp_stage_bc("DEMUX_OVERSHOOT", d);
                s_last_bc_ms = now;
            }
            return;
        }

        usleep(sleep_us);
    }
}

void *demux_thread_func(void *arg) {
    (void)arg;

    AVPacket *pkt =
        av_packet_alloc();

    if (!pkt) {
        /* Nothing will ever fill the queue, so do not leave the decode
         * threads parked on a hold that can no longer be cleared. */
        pb_prebuffer_hold = 0;
        return NULL;
    }

    /* Deadline for the pre-buffer, measured from when this thread actually
     * starts reading rather than from when it was created. */
    const long long prebuffer_deadline_ms = now_ms() + (long long)pb_prebuffer_max_ms;

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
            prebuffer_check(prebuffer_deadline_ms, 1);
            usleep(5000);
            continue;
        }

        video_decode_done = 0;
        prebuffer_check(prebuffer_deadline_ms, 0);

        if (
            pkt->stream_index ==
            video_stream_index
        ) {
            /*
             * Do not drop non-keyframes in demux — that freezes for a full GOP
             * (often every 1–2s). Cap queue by waiting only.
             */
            demux_wait_for_room(&video_packet_queue, video_packet_cap,
                                &audio_packet_queue, audio_packet_cap,
                                prebuffer_deadline_ms,
                                playback_profile >= 3 ? 300 : 500);

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
            demux_wait_for_room(&audio_packet_queue, audio_packet_cap,
                                &video_packet_queue, video_packet_cap,
                                prebuffer_deadline_ms, 1000);

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
            prospero_subtitle_wants_stream(
                pkt->stream_index
            )
        ) {
            if (
                pkt->stream_index ==
                prospero_embedded_subtitle_stream_index
            ) {
                dbg_sub_demuxed++;
            }

            prospero_embedded_subtitle_decode_packet(
                pkt
            );
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return NULL;
}

