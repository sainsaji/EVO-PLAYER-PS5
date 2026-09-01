/*
 * evo_audio_out.c — media audio output path: decode → resample → AudioOut.
 *
 * Verbatim move of the audio-out ring / output thread, the audio sample mix,
 * the audio decode thread, and the AUDIO_TRACK_SWITCH region from main.c
 * (Track A step A3 of docs/modularisation-plan.md). No behaviour change: the
 * only edits are `static` → external linkage on the state main.c still touches
 * and the transitional extern block below for the playback-core / subtitle /
 * resume globals this code reads. All of that collapses into the evo_pb_*()
 * façade at A8.
 */
#include "evo_audio_out.h"

#include <math.h>
#include <string.h>
#include <unistd.h>

#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "evo_packet_queue.h"
#include "evo_audio_resample.h"

#ifndef SCREEN_PLAYER
#define SCREEN_PLAYER 2
#endif

int sceAudioOutOutput(int handle, const void *ptr);

/* ---------------------------------------------------------------------------
 * TRANSITIONAL: playback-core, subtitle and resume state still owned by
 * main.c. Replaced by evo_pb_*() / evo_subtitle_*() accessors at A8 / A4.
 * ------------------------------------------------------------------------ */
extern int               screen;
extern int               player_paused;
extern double            video_clock_seconds;
extern double            first_video_pts_seconds;
extern int               video_decode_done;
extern int               audio_stream_index;
extern int               evo_audio_channels;
extern AVFormatContext  *play_fmt;
extern AVCodecContext   *audio_ctx;
extern PacketQueue       audio_packet_queue;
extern double            resume_base_offset_seconds;
extern double            media_duration_sec;
extern double            requested_resume_seek_pos;
extern long long         controls_last_used_ms;
extern int               prospero_subtitle_enabled;
extern int               prospero_subtitle_use_external;
extern int               prospero_subtitle_count;
extern int               prospero_embedded_subtitle_stream_index;
extern int               prospero_subtitle_requested_stream;
extern char              current_media_path[512];

int      start_video_playback(const char *path);
void     toast(const char *title, const char *msg);
long long now_ms(void);

/* ---------------------------------------------------------------------------
 * Audio-out session state (exported via evo_audio_out.h).
 * ------------------------------------------------------------------------ */
int audio_handle = -1;
static int16_t audio_accum[2048 * EVO_AUDIO_MAX_CH];
int audio_accum_pos = 0;

static int16_t audio_queue[AUDIO_QUEUE_BLOCKS][AUDIO_BLOCK_SAMPLES * EVO_AUDIO_MAX_CH];
volatile int audio_queue_read = 0;
volatile int audio_queue_write = 0;
volatile int audio_queue_count = 0;
volatile long long audio_samples_played = 0;
volatile long long audio_samples_decoded = 0;
volatile double audio_clock_seconds = 0.0;
volatile double audio_pts_seconds = 0.0;
double first_audio_pts_seconds = -1.0;

int detected_audio_rate = 48000;
volatile int audio_thread_running = 0;
pthread_t audio_thread;

volatile int audio_decode_thread_running = 0;
pthread_t audio_decode_thread;


static void audio_queue_push(int16_t *buf) {
    int spins = 0;
    /* Wait for space instead of dropping — drops cause choppy audio on UHD */
    while (audio_queue_count >= AUDIO_QUEUE_BLOCKS &&
           audio_decode_thread_running && spins < 200) {
        usleep(500);
        spins++;
    }
    if (audio_queue_count >= AUDIO_QUEUE_BLOCKS)
        return;

    memcpy(audio_queue[audio_queue_write], buf,
           (size_t)AUDIO_BLOCK_SAMPLES * evo_audio_channels * sizeof(int16_t));
    audio_queue_write = (audio_queue_write + 1) % AUDIO_QUEUE_BLOCKS;
    audio_queue_count++;
}

void *audio_output_thread(void *arg) {
    static int16_t silence[AUDIO_BLOCK_SAMPLES * EVO_AUDIO_MAX_CH];
    while (audio_thread_running) {
        if (screen == 2 && !player_paused && audio_handle >= 1) {
            /*
             * Compare like-for-like: audio_clock is from t=0 of this session,
             * video must be relative to first video PTS (not absolute PTS).
             * Absolute compare freezes easy 720p/YouTube when clocks diverge.
             */
            double video_rel = video_clock_seconds;
            if (first_video_pts_seconds >= 0.0)
                video_rel = video_clock_seconds - first_video_pts_seconds;
            if (video_rel < 0.0)
                video_rel = 0.0;

            if (video_rel > 0.1 &&
                audio_clock_seconds > video_rel + 0.50) {
                usleep(2000);
                continue;
            }
            if (audio_queue_count > 0) {
                sceAudioOutOutput(audio_handle, audio_queue[audio_queue_read]);
                audio_samples_played += AUDIO_BLOCK_SAMPLES;
                audio_clock_seconds = (double)audio_samples_played / 48000.0;
                audio_queue_read = (audio_queue_read + 1) % AUDIO_QUEUE_BLOCKS;
                audio_queue_count--;
            } else {
                /*
                 * Soft underrun: silence without advancing media clock.
                 * Video wait loop must break if audio stays stuck (see decode).
                 */
                sceAudioOutOutput(audio_handle, silence);
            }
        } else {
            usleep(200);
        }
    }
    return NULL;
}

static float audio_get_sample(AVFrame *af, int ch, int i) {
    int channels = af->ch_layout.nb_channels;
    if (channels <= 0) channels = 2;

    if (ch >= channels) ch = channels - 1;
    if (ch < 0) ch = 0;

    if (af->format == AV_SAMPLE_FMT_FLTP) {
        float *d = (float*)af->data[ch];
        return d ? d[i] : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_FLT) {
        float *d = (float*)af->data[0];
        return d ? d[i * channels + ch] : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_S16P) {
        int16_t *d = (int16_t*)af->data[ch];
        return d ? ((float)d[i] / 32768.0f) : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_S16) {
        int16_t *d = (int16_t*)af->data[0];
        return d ? ((float)d[i * channels + ch] / 32768.0f) : 0.0f;
    }

    return 0.0f;
}

static int16_t audio_float_to_s16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 30000.0f);
}

static void mix_audio_frame_to_queue(
    AVFrame *frame
) {
    if (
        !frame ||
        frame->nb_samples <= 0
    ) {
        return;
    }

    if (
        !prospero_audio_resampler_configure(
            frame,
            evo_audio_channels,
            audio_ctx
        )
    ) {
        return;
    }

    int input_rate =
        frame->sample_rate > 0
            ? frame->sample_rate
            : prospero_audio_resampler_input_rate();

    int64_t delay =
        swr_get_delay(
            prospero_audio_resampler_ctx(),
            input_rate
        );

    int output_capacity =
        (int)av_rescale_rnd(
            delay + frame->nb_samples,
            PROSPERO_AUDIO_OUTPUT_RATE,
            input_rate,
            AV_ROUND_UP
        );

    if (output_capacity <= 0) {
        return;
    }

    uint8_t *output_buffer = NULL;
    int output_linesize = 0;

    if (
        av_samples_alloc(
            &output_buffer,
            &output_linesize,
            evo_audio_channels,   /* EVO: was hardcoded 2 */
            output_capacity,
            AV_SAMPLE_FMT_S16,
            0
        ) < 0
    ) {
        return;
    }

    uint8_t *output_planes[1] = {
        output_buffer
    };

    int converted =
        swr_convert(
            prospero_audio_resampler_ctx(),
            output_planes,
            output_capacity,
            (const uint8_t * const *)
                frame->extended_data,
            frame->nb_samples
        );

    if (converted > 0) {
        int16_t *samples =
            (int16_t *)output_buffer;

        for (
            int index = 0;
            index < converted &&
            audio_decode_thread_running;
            index++
        ) {
            /* EVO: copy every channel, not just L/R. Both buffers are
             * interleaved with the same channel count, so this is a straight
             * per-frame copy of evo_audio_channels samples. */
            for (int c = 0; c < evo_audio_channels; c++) {
                audio_accum[audio_accum_pos * evo_audio_channels + c] =
                    samples[index * evo_audio_channels + c];
            }

            audio_accum_pos++;
            audio_samples_decoded++;

            if (
                audio_accum_pos >=
                AUDIO_BLOCK_SAMPLES
            ) {
                while (
                    audio_decode_thread_running &&
                    audio_queue_count >=
                        AUDIO_QUEUE_BLOCKS - 2
                ) {
                    usleep(1000);
                }

                if (!audio_decode_thread_running) {
                    break;
                }

                audio_queue_push(audio_accum);
                audio_accum_pos = 0;
            }
        }
    }

    av_freep(&output_buffer);
}


void *audio_decode_thread_func(void *arg) {
    AVFrame *af = av_frame_alloc();
    if (!af) return NULL;

    while (audio_decode_thread_running) {
        if (player_paused || screen != 2) {
            usleep(1000);
            continue;
        }

        while (audio_decode_thread_running && audio_queue_count > 10) {
            usleep(1000);
        }

        AVPacket *pkt = packet_queue_pop(&audio_packet_queue);
        if (!pkt) {
            /*
             * Do not terminate at EOF. The demux thread stays alive
             * so an in-place backward seek can refill this queue.
             */
            usleep(
                video_decode_done
                    ? 5000
                    : 1000
            );

            continue;
        }

        if (avcodec_send_packet(audio_ctx, pkt) == 0) {
            while (audio_decode_thread_running && avcodec_receive_frame(audio_ctx, af) == 0) {
                if (af->pts != AV_NOPTS_VALUE && play_fmt && audio_stream_index >= 0) {
                    audio_pts_seconds = af->pts * av_q2d(play_fmt->streams[audio_stream_index]->time_base);
                    if (first_audio_pts_seconds < 0.0)
                        first_audio_pts_seconds = audio_pts_seconds;
                }

                mix_audio_frame_to_queue(af);
                av_frame_unref(af);
            }
        }

        av_packet_free(&pkt);
    }

    av_frame_free(&af);
    return NULL;
}


/* PROSPERO_AUDIO_TRACK_SWITCH_START */

#define PROSPERO_AUDIO_TRACK_LIMIT 32

int prospero_audio_requested_stream = -1;
char prospero_audio_active_label[128] = {0};


static int prospero_audio_collect_streams(
    AVFormatContext *format,
    int *indexes,
    int maximum
) {
    if (!format || !indexes || maximum <= 0) {
        return 0;
    }

    int count = 0;

    for (
        unsigned int index = 0;
        index < format->nb_streams &&
        count < maximum;
        index++
    ) {
        AVStream *stream =
            format->streams[index];

        if (
            !stream ||
            !stream->codecpar ||
            stream->codecpar->codec_type !=
                AVMEDIA_TYPE_AUDIO
        ) {
            continue;
        }

        if (
            !avcodec_find_decoder(
                stream->codecpar->codec_id
            )
        ) {
            continue;
        }

        indexes[count++] =
            (int)index;
    }

    return count;
}


void prospero_audio_build_label(
    AVFormatContext *format,
    int selected_stream,
    char *output,
    size_t output_size
) {
    if (!output || output_size == 0) {
        return;
    }

    snprintf(
        output,
        output_size,
        "UNKNOWN AUDIO"
    );

    if (
        !format ||
        selected_stream < 0 ||
        selected_stream >=
            (int)format->nb_streams
    ) {
        return;
    }

    int indexes[
        PROSPERO_AUDIO_TRACK_LIMIT
    ];

    int count =
        prospero_audio_collect_streams(
            format,
            indexes,
            PROSPERO_AUDIO_TRACK_LIMIT
        );

    int ordinal = 0;

    for (int index = 0; index < count; index++) {
        if (indexes[index] == selected_stream) {
            ordinal = index + 1;
            break;
        }
    }

    AVStream *stream =
        format->streams[selected_stream];

    AVCodecParameters *parameters =
        stream->codecpar;

    const AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    const char *language_text =
        (
            language &&
            language->value &&
            language->value[0]
        )
            ? language->value
            : "und";

    const char *codec_name =
        avcodec_get_name(
            parameters->codec_id
        );

    int channels =
        parameters->ch_layout.nb_channels;

    snprintf(
        output,
        output_size,
        "%d/%d  %s  %s  %dCH",
        ordinal,
        count,
        language_text,
        codec_name
            ? codec_name
            : "unknown",
        channels
    );
}


void prospero_audio_cycle_track(void)
{
    if (
        screen != SCREEN_PLAYER ||
        !play_fmt
    ) {
        return;
    }

    int indexes[
        PROSPERO_AUDIO_TRACK_LIMIT
    ];

    int count =
        prospero_audio_collect_streams(
            play_fmt,
            indexes,
            PROSPERO_AUDIO_TRACK_LIMIT
        );

    if (count <= 0) {
        toast(
            "AUDIO TRACK",
            "NO SUPPORTED AUDIO"
        );

        return;
    }

    if (count == 1) {
        prospero_audio_build_label(
            play_fmt,
            indexes[0],
            prospero_audio_active_label,
            sizeof(prospero_audio_active_label)
        );

        toast(
            "AUDIO TRACK",
            "ONLY ONE TRACK"
        );

        return;
    }

    int current_slot = 0;

    for (int index = 0; index < count; index++) {
        if (
            indexes[index] ==
            audio_stream_index
        ) {
            current_slot = index;
            break;
        }
    }

    int next_slot =
        (current_slot + 1) % count;

    int next_stream =
        indexes[next_slot];

    int restore_paused =
        player_paused;

    int restore_subtitles =
        prospero_subtitle_enabled;

    int subtitle_request = -2;

    if (
        prospero_subtitle_use_external &&
        prospero_subtitle_count > 0
    ) {
        subtitle_request = -1;
    } else if (
        !prospero_subtitle_use_external &&
        prospero_embedded_subtitle_stream_index >= 0
    ) {
        subtitle_request =
            prospero_embedded_subtitle_stream_index;
    }

    double position =
        resume_base_offset_seconds +
        (
            audio_stream_index >= 0
                ? audio_clock_seconds
                : video_clock_seconds
        );

    if (position < 0.0) {
        position = 0.0;
    }

    if (
        media_duration_sec > 0.0 &&
        position > media_duration_sec
    ) {
        position = media_duration_sec;
    }

    char playback_path[1024];

    snprintf(
        playback_path,
        sizeof(playback_path),
        "%s",
        current_media_path
    );

    prospero_audio_requested_stream =
        next_stream;

    prospero_subtitle_requested_stream =
        subtitle_request;

    /*
     * Playback startup subtracts one second for keyframe preroll.
     */
    requested_resume_seek_pos =
        position > 3.0
            ? position + 1.0
            : position;

    resume_base_offset_seconds =
        position;

    if (
        !start_video_playback(
            playback_path
        )
    ) {
        prospero_audio_requested_stream = -1;

        toast(
            "AUDIO TRACK",
            "SWITCH FAILED"
        );

        return;
    }

    player_paused =
        restore_paused;

    prospero_subtitle_enabled =
        restore_subtitles;

    prospero_audio_build_label(
        play_fmt,
        audio_stream_index,
        prospero_audio_active_label,
        sizeof(prospero_audio_active_label)
    );

    controls_last_used_ms =
        now_ms();

    toast(
        "AUDIO TRACK",
        prospero_audio_active_label
    );
}

/* PROSPERO_AUDIO_TRACK_SWITCH_END */
