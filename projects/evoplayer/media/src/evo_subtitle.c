/*
 * evo_subtitle.c — subtitle engine: embedded (MKV text tracks) + external SRT.
 *
 * Verbatim move of the EMBEDDED_SUBTITLE_MODULE, SRT_MODULE and
 * SUBTITLE_CONTROLS regions from main.c (Track A step A4 of
 * docs/modularisation-plan.md). The only edits are `static` -> external
 * linkage on the handful of symbols main.c still touches (see evo_subtitle.h)
 * and the transitional extern block below.
 */
#include "evo_subtitle.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>

#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>

#include "evo_audio_out.h"   /* audio_handle, audio_clock_seconds */

#ifndef SCREEN_PLAYER
#define SCREEN_PLAYER 2
#endif

/* ---------------------------------------------------------------------------
 * TRANSITIONAL: playback-core / resume state still owned by main.c.
 * Replaced by the evo_pb_*() façade at A8. prospero_subtitle_draw() stays in
 * main.c — it is welded to the 335k-line font-atlas headers and rr_text.
 * ------------------------------------------------------------------------ */
extern int              screen;
extern int              player_paused;
extern double           video_clock_seconds;
extern AVFormatContext *play_fmt;
extern AVCodecContext  *audio_ctx;
extern char             current_media_path[512];
extern double           resume_base_offset_seconds;
extern double           requested_resume_seek_pos;
extern long long        controls_last_used_ms;

void      toast(const char *title, const char *msg);
long long now_ms(void);
int       start_video_playback(const char *path);

/* Defined in the SRT section below; used by the embedded section above it. */
static void prospero_subtitle_clean_line(const char *input, char *output,
                                         size_t output_size);
static void prospero_subtitle_append_text(char *destination,
                                          size_t destination_size,
                                          const char *line);

/* Was a standalone static in main.c (near the browser-preview globals). */
int prospero_subtitle_delay_ms = 0;

/* PROSPERO_EMBEDDED_SUBTITLE_MODULE_START */

#define PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES 256
#define PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE 768

typedef struct {
    double start_seconds;
    double end_seconds;

    char text[
        PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
    ];
} ProsperoEmbeddedSubtitleCue;


/*
 * Subtitle pipeline counters. The trace already reported "a stream is
 * selected" and "no cues exist", which is a gap wide enough to hold every
 * stage in between. These split it: demuxed -> reached the decoder -> text
 * survived cleaning -> stored.
 */
int dbg_sub_demuxed;   /* packets av_read_frame gave us on the track */
int dbg_sub_entered;   /* packets that got past the decoder's guard  */
int dbg_sub_blank;     /* decoded but the text cleaned away to empty */
int dbg_sub_added;     /* add_cue calls                              */
int dbg_sub_cid;       /* codec id actually selected                 */

AVCodecContext *
prospero_embedded_subtitle_ctx = NULL;

int
prospero_embedded_subtitle_stream_index = -1;

/* PROSPERO_DIRECT_SUBRIP_START */

static enum AVCodecID
prospero_embedded_subtitle_codec_id =
    AV_CODEC_ID_NONE;

/* PROSPERO_DIRECT_SUBRIP_END */



/* PROSPERO_SUBTITLE_SELECTION_STATE_START */

/*
 * -2 = automatic
 * -1 = external matching SRT
 * >=0 = exact embedded MKV subtitle stream
 */
int prospero_subtitle_requested_stream = -2;

/*
 * Zero renders the active embedded stream.
 * One renders the matching external SRT.
 */
int prospero_subtitle_use_external = 0;

/* PROSPERO_SUBTITLE_SELECTION_STATE_END */




static ProsperoEmbeddedSubtitleCue
prospero_embedded_subtitle_cues[
    PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES
];

static int
prospero_embedded_subtitle_head = 0;

int
prospero_embedded_subtitle_count = 0;

static pthread_mutex_t
prospero_embedded_subtitle_mutex =
    PTHREAD_MUTEX_INITIALIZER;


int prospero_embedded_subtitle_supported(
    enum AVCodecID codec_id
) {
    return (
        codec_id == AV_CODEC_ID_SUBRIP ||
        codec_id == AV_CODEC_ID_ASS ||
        codec_id == AV_CODEC_ID_SSA ||
        codec_id == AV_CODEC_ID_WEBVTT
    );
}


void prospero_embedded_subtitle_reset(void) {
    pthread_mutex_lock(
        &prospero_embedded_subtitle_mutex
    );

    prospero_embedded_subtitle_head = 0;
    prospero_embedded_subtitle_count = 0;

    pthread_mutex_unlock(
        &prospero_embedded_subtitle_mutex
    );
}


void prospero_embedded_subtitle_close(void) {
    prospero_embedded_subtitle_reset();

    if (prospero_embedded_subtitle_ctx) {
        avcodec_free_context(
            &prospero_embedded_subtitle_ctx
        );
    }

    prospero_embedded_subtitle_stream_index =
        -1;

    prospero_embedded_subtitle_codec_id =
        AV_CODEC_ID_NONE;
}


/*
 * How many cues a subtitle track claims to hold, or -1 when it does not say.
 *
 * mkvmerge writes per-track STATISTICS tags, and the key is suffixed with the
 * language - NUMBER_OF_FRAMES-eng - so the lookup has to ignore the suffix.
 *
 * This matters more than it looks. Release groups ship a vanity track that is
 * flagged "default", is tagged English, and contains two cues forty minutes
 * in. It wins every metadata-based contest against the real subtitle track
 * and then displays nothing, which is indistinguishable from subtitles being
 * broken. The count is the only field that tells them apart before playback.
 */
int prospero_subtitle_declared_cues(
    AVStream *stream
) {
    if (!stream) {
        return -1;
    }

    AVDictionaryEntry *frames =
        av_dict_get(
            stream->metadata,
            "NUMBER_OF_FRAMES",
            NULL,
            AV_DICT_IGNORE_SUFFIX
        );

    if (
        !frames ||
        !frames->value ||
        !frames->value[0]
    ) {
        return -1;
    }

    long parsed =
        strtol(
            frames->value,
            NULL,
            10
        );

    if (
        parsed < 0 ||
        parsed > 1000000
    ) {
        return -1;
    }

    return (int)parsed;
}




static int prospero_embedded_subtitle_score_stream(
    AVStream *stream
) {
    if (
        !stream ||
        !stream->codecpar ||
        !prospero_embedded_subtitle_supported(
            stream->codecpar->codec_id
        )
    ) {
        return -100000;
    }

    int score = 10;

    AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    if (language && language->value) {
        if (
            strcasecmp(
                language->value,
                "eng"
            ) == 0 ||
            strcasecmp(
                language->value,
                "en"
            ) == 0 ||
            strcasecmp(
                language->value,
                "english"
            ) == 0
        ) {
            score += 100;
        }
    }

    if (
        stream->disposition &
        AV_DISPOSITION_DEFAULT
    ) {
        score += 40;
    }

    /*
     * Cue count outranks every metadata signal, because it is the only one
     * that reports what the track actually contains rather than what it
     * claims to be. A near-empty track loses even when it is English and
     * flagged default; a rich one gets a modest tie-break bonus.
     */
    {
        int cues =
            prospero_subtitle_declared_cues(
                stream
            );

        if (
            cues >= 0 &&
            cues < PROSPERO_SUBTITLE_MIN_USEFUL_CUES
        ) {
            score -= 1000;
        } else if (cues > 0) {
            score += cues > 50 ? 20 : 5;
        }
    }

    if (
        stream->disposition &
        AV_DISPOSITION_FORCED
    ) {
        score += 25;
    }

    return score;
}


int prospero_embedded_subtitle_open(
    AVFormatContext *format
) {
    prospero_embedded_subtitle_close();

    /*
     * A matching external SRT is treated as the selected track.
     */
    if (!format) {
        return 0;
    }

    int best_stream = -1;
    int best_score = -100000;
    int found_pgs = 0;

    for (
        unsigned int index = 0;
        index < format->nb_streams;
        index++
    ) {
        AVStream *stream =
            format->streams[index];

        if (
            !stream ||
            !stream->codecpar ||
            stream->codecpar->codec_type !=
                AVMEDIA_TYPE_SUBTITLE
        ) {
            continue;
        }

        if (
            stream->codecpar->codec_id ==
            AV_CODEC_ID_HDMV_PGS_SUBTITLE
        ) {
            found_pgs = 1;
            continue;
        }

        int score =
            prospero_embedded_subtitle_score_stream(
                stream
            );

        if (score > best_score) {
            best_score = score;
            best_stream = (int)index;
        }
    }

    
    /*
     * D-pad track cycling may request a specific embedded stream.
     */
    if (
        prospero_subtitle_requested_stream >= 0
    ) {
        int requested =
            prospero_subtitle_requested_stream;

        if (
            requested <
            (int)format->nb_streams
        ) {
            AVStream *requested_stream =
                format->streams[requested];

            if (
                requested_stream &&
                requested_stream->codecpar &&
                requested_stream->codecpar->codec_type ==
                    AVMEDIA_TYPE_SUBTITLE &&
                prospero_embedded_subtitle_supported(
                    requested_stream->codecpar->codec_id
                )
            ) {
                best_stream = requested;
            }
        }
    }

if (best_stream < 0) {
        if (found_pgs) {
            toast(
                "SUBTITLES",
                "PGS NOT SUPPORTED YET"
            );
        }

        return 0;
    }

    AVStream *stream =
        format->streams[best_stream];

    enum AVCodecID codec_id =
        stream->codecpar->codec_id;

    prospero_embedded_subtitle_stream_index =
        best_stream;

    prospero_embedded_subtitle_codec_id =
        codec_id;

    dbg_sub_cid = (int)codec_id;

    prospero_embedded_subtitle_reset();

    /*
     * Matroska SubRip packets already contain plain subtitle text.
     * Decode them directly when the PS5 FFmpeg build does not include
     * a registered SubRip decoder.
     */
    if (codec_id != AV_CODEC_ID_SUBRIP) {
        const AVCodec *decoder =
            avcodec_find_decoder(
                codec_id
            );

        if (!decoder) {
            prospero_embedded_subtitle_stream_index =
                -1;

            prospero_embedded_subtitle_codec_id =
                AV_CODEC_ID_NONE;

            toast(
                "SUBTITLES",
                "TEXT DECODER NOT FOUND"
            );

            return 0;
        }

        AVCodecContext *context =
            avcodec_alloc_context3(
                decoder
            );

        if (!context) {
            prospero_embedded_subtitle_stream_index =
                -1;

            prospero_embedded_subtitle_codec_id =
                AV_CODEC_ID_NONE;

            return 0;
        }

        if (
            avcodec_parameters_to_context(
                context,
                stream->codecpar
            ) < 0
        ) {
            avcodec_free_context(
                &context
            );

            prospero_embedded_subtitle_stream_index =
                -1;

            prospero_embedded_subtitle_codec_id =
                AV_CODEC_ID_NONE;

            return 0;
        }

        context->pkt_timebase =
            stream->time_base;

        if (
            avcodec_open2(
                context,
                decoder,
                NULL
            ) < 0
        ) {
            avcodec_free_context(
                &context
            );

            prospero_embedded_subtitle_stream_index =
                -1;

            prospero_embedded_subtitle_codec_id =
                AV_CODEC_ID_NONE;

            toast(
                "SUBTITLES",
                "TEXT DECODER FAILED"
            );

            return 0;
        }

        prospero_embedded_subtitle_ctx =
            context;
    }

    const char *codec_name =
        avcodec_get_name(
            codec_id
        );

    const char *language_name =
        "UNSPECIFIED";

    AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    if (
        language &&
        language->value &&
        language->value[0]
    ) {
        language_name =
            language->value;
    }

    char message[128];

    snprintf(
        message,
        sizeof(message),
        "%s / %s",
        language_name,
        codec_name
            ? codec_name
            : "TEXT"
    );

    toast(
        "EMBEDDED SUBTITLES",
        message
    );

    return 1;
}


static const char *
prospero_embedded_subtitle_ass_payload(
    const char *ass
) {
    if (!ass) {
        return NULL;
    }

    int fields_to_skip =
        strncmp(
            ass,
            "Dialogue:",
            9
        ) == 0
            ? 9
            : 8;

    const char *cursor = ass;
    int comma_count = 0;

    while (*cursor) {
        if (*cursor == ',') {
            comma_count++;

            if (
                comma_count >=
                fields_to_skip
            ) {
                return cursor + 1;
            }
        }

        cursor++;
    }

    return ass;
}


static void prospero_embedded_subtitle_extract_text(
    AVSubtitle *subtitle,
    char *output,
    size_t output_size
) {
    if (
        !subtitle ||
        !output ||
        output_size == 0
    ) {
        return;
    }

    output[0] = 0;

    for (
        unsigned int index = 0;
        index < subtitle->num_rects;
        index++
    ) {
        AVSubtitleRect *rectangle =
            subtitle->rects[index];

        if (!rectangle) {
            continue;
        }

        const char *raw_text = NULL;

        if (
            rectangle->text &&
            rectangle->text[0]
        ) {
            raw_text =
                rectangle->text;
        } else if (
            rectangle->ass &&
            rectangle->ass[0]
        ) {
            raw_text =
                prospero_embedded_subtitle_ass_payload(
                    rectangle->ass
                );
        }

        if (!raw_text || !raw_text[0]) {
            continue;
        }

        char cleaned[
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ];

        prospero_subtitle_clean_line(
            raw_text,
            cleaned,
            sizeof(cleaned)
        );

        if (cleaned[0]) {
            prospero_subtitle_append_text(
                output,
                output_size,
                cleaned
            );
        }
    }
}


static void prospero_embedded_subtitle_add_cue(
    double start_seconds,
    double end_seconds,
    const char *text
) {
    if (
        !text ||
        !text[0] ||
        end_seconds <= start_seconds
    ) {
        return;
    }

    pthread_mutex_lock(
        &prospero_embedded_subtitle_mutex
    );

    if (
        prospero_embedded_subtitle_count >=
        PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES
    ) {
        prospero_embedded_subtitle_head =
            (
                prospero_embedded_subtitle_head +
                1
            ) %
            PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES;

        prospero_embedded_subtitle_count--;
    }

    int write_index =
        (
            prospero_embedded_subtitle_head +
            prospero_embedded_subtitle_count
        ) %
        PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES;

    ProsperoEmbeddedSubtitleCue *cue =
        &prospero_embedded_subtitle_cues[
            write_index
        ];

    cue->start_seconds =
        start_seconds;

    cue->end_seconds =
        end_seconds;

    snprintf(
        cue->text,
        sizeof(cue->text),
        "%s",
        text
    );

    prospero_embedded_subtitle_count++;

    pthread_mutex_unlock(
        &prospero_embedded_subtitle_mutex
    );
}


void prospero_embedded_subtitle_decode_packet(
    AVPacket *packet
) {
    if (
        !packet ||
        packet->stream_index !=
            prospero_embedded_subtitle_stream_index ||
        !play_fmt ||
        prospero_embedded_subtitle_stream_index < 0
    ) {
        return;
    }

    dbg_sub_entered++;

    AVStream *stream =
        play_fmt->streams[
            prospero_embedded_subtitle_stream_index
        ];

    int64_t timestamp =
        packet->pts != AV_NOPTS_VALUE
            ? packet->pts
            : packet->dts;

    double base_seconds = 0.0;

    if (timestamp != AV_NOPTS_VALUE) {
        base_seconds =
            timestamp *
            av_q2d(
                stream->time_base
            );
    }

    double packet_duration = 0.0;

    if (packet->duration > 0) {
        packet_duration =
            packet->duration *
            av_q2d(
                stream->time_base
            );
    }

    if (packet_duration <= 0.0) {
        packet_duration = 5.0;
    }

    /*
     * Direct Matroska SubRip path.
     */
    if (
        prospero_embedded_subtitle_codec_id ==
        AV_CODEC_ID_SUBRIP
    ) {
        if (
            !packet->data ||
            packet->size <= 0
        ) {
            return;
        }

        size_t copy_size =
            (size_t)packet->size;

        if (
            copy_size >=
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ) {
            copy_size =
                PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE - 1;
        }

        char raw_text[
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ];

        memcpy(
            raw_text,
            packet->data,
            copy_size
        );

        raw_text[copy_size] = 0;

        char cleaned[
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ];

        prospero_subtitle_clean_line(
            raw_text,
            cleaned,
            sizeof(cleaned)
        );

        if (cleaned[0]) {
            dbg_sub_added++;

            prospero_embedded_subtitle_add_cue(
                base_seconds,
                base_seconds + packet_duration,
                cleaned
            );
        } else {
            dbg_sub_blank++;
        }

        return;
    }

    if (!prospero_embedded_subtitle_ctx) {
        return;
    }

    AVSubtitle subtitle;

    memset(
        &subtitle,
        0,
        sizeof(subtitle)
    );

    subtitle.pts =
        AV_NOPTS_VALUE;

    int got_subtitle = 0;

    int result =
        avcodec_decode_subtitle2(
            prospero_embedded_subtitle_ctx,
            &subtitle,
            &got_subtitle,
            packet
        );

    if (
        result < 0 ||
        !got_subtitle
    ) {
        avsubtitle_free(
            &subtitle
        );

        return;
    }

    if (
        subtitle.pts !=
        AV_NOPTS_VALUE
    ) {
        base_seconds =
            (double)subtitle.pts /
            (double)AV_TIME_BASE;
    }

    double start_seconds =
        base_seconds +
        subtitle.start_display_time /
            1000.0;

    double end_seconds =
        base_seconds +
        subtitle.end_display_time /
            1000.0;

    if (
        end_seconds <= start_seconds ||
        end_seconds - start_seconds >
            120.0
    ) {
        end_seconds =
            start_seconds +
            packet_duration;
    }

    if (end_seconds <= start_seconds) {
        end_seconds =
            start_seconds + 5.0;
    }

    char text[
        PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
    ];

    prospero_embedded_subtitle_extract_text(
        &subtitle,
        text,
        sizeof(text)
    );

    if (text[0]) {
        prospero_embedded_subtitle_add_cue(
            start_seconds,
            end_seconds,
            text
        );
    }

    avsubtitle_free(
        &subtitle
    );
}


int prospero_embedded_subtitle_text_at(
    double position,
    char *output,
    size_t output_size
) {
    if (
        !output ||
        output_size == 0
    ) {
        return 0;
    }

    output[0] = 0;

    pthread_mutex_lock(
        &prospero_embedded_subtitle_mutex
    );

    const ProsperoEmbeddedSubtitleCue *
        selected = NULL;

    for (
        int offset = 0;
        offset <
            prospero_embedded_subtitle_count;
        offset++
    ) {
        int index =
            (
                prospero_embedded_subtitle_head +
                offset
            ) %
            PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES;

        const ProsperoEmbeddedSubtitleCue *cue =
            &prospero_embedded_subtitle_cues[
                index
            ];

        if (
            position >= cue->start_seconds &&
            position <= cue->end_seconds
        ) {
            if (
                !selected ||
                cue->start_seconds >
                    selected->start_seconds
            ) {
                selected = cue;
            }
        }
    }

    if (selected) {
        snprintf(
            output,
            output_size,
            "%s",
            selected->text
        );
    }

    pthread_mutex_unlock(
        &prospero_embedded_subtitle_mutex
    );

    return output[0] != 0;
}

/* PROSPERO_EMBEDDED_SUBTITLE_MODULE_END */
/* PROSPERO_SRT_MODULE_START */

#define PROSPERO_SUBTITLE_MAX_CUES 2048
/* PROSPERO_SUBTITLE_{TEXT_SIZE,MAX_LINES,LINE_SIZE} and the ProsperoSubtitleCue
 * type are in evo_subtitle.h now (prospero_subtitle_draw in main.c needs them). */

static ProsperoSubtitleCue prospero_subtitle_cues[
    PROSPERO_SUBTITLE_MAX_CUES
];

int prospero_subtitle_count = 0;
int prospero_subtitle_enabled = 1;
int prospero_subtitle_face = 2;  /* 1 SUB (SMALL), 2 MENU (MEDIUM/DEFAULT), 3 TITLE (LARGE) */

static char prospero_subtitle_path[512] = {0};


void prospero_subtitle_clear(void) {
    prospero_subtitle_count = 0;
    prospero_subtitle_path[0] = 0;
}


void prospero_subtitle_trim(
    char *text
) {
    if (!text) {
        return;
    }

    size_t length =
        strlen(text);

    while (
        length > 0 &&
        (
            text[length - 1] == '\r' ||
            text[length - 1] == '\n' ||
            text[length - 1] == ' ' ||
            text[length - 1] == '\t'
        )
    ) {
        text[--length] = 0;
    }

    size_t start = 0;

    while (
        text[start] == ' ' ||
        text[start] == '\t'
    ) {
        start++;
    }

    if (start > 0) {
        memmove(
            text,
            text + start,
            strlen(text + start) + 1
        );
    }
}


static int prospero_subtitle_parse_timing(
    const char *line,
    double *start_seconds,
    double *end_seconds
) {
    int start_hour = 0;
    int start_minute = 0;
    int start_second = 0;
    int start_millisecond = 0;

    int end_hour = 0;
    int end_minute = 0;
    int end_second = 0;
    int end_millisecond = 0;

    char start_separator = 0;
    char end_separator = 0;

    int matched =
        sscanf(
            line,
            "%d:%d:%d%c%d --> %d:%d:%d%c%d",
            &start_hour,
            &start_minute,
            &start_second,
            &start_separator,
            &start_millisecond,
            &end_hour,
            &end_minute,
            &end_second,
            &end_separator,
            &end_millisecond
        );

    if (matched != 10) {
        return 0;
    }

    if (
        (
            start_separator != ',' &&
            start_separator != '.'
        ) ||
        (
            end_separator != ',' &&
            end_separator != '.'
        )
    ) {
        return 0;
    }

    *start_seconds =
        start_hour * 3600.0 +
        start_minute * 60.0 +
        start_second +
        start_millisecond / 1000.0;

    *end_seconds =
        end_hour * 3600.0 +
        end_minute * 60.0 +
        end_second +
        end_millisecond / 1000.0;

    return (
        *end_seconds >=
        *start_seconds
    );
}


static void prospero_subtitle_clean_line(
    const char *input,
    char *output,
    size_t output_size
) {
    if (
        !input ||
        !output ||
        output_size == 0
    ) {
        return;
    }

    size_t write_index = 0;
    int inside_angle_tag = 0;
    int inside_brace_tag = 0;

    for (
        size_t index = 0;
        input[index] &&
        write_index + 1 < output_size;
        index++
    ) {
        unsigned char value =
            (unsigned char)input[index];

        if (value == '<') {
            inside_angle_tag = 1;
            continue;
        }

        if (
            inside_angle_tag &&
            value == '>'
        ) {
            inside_angle_tag = 0;
            continue;
        }

        if (value == '{') {
            inside_brace_tag = 1;
            continue;
        }

        if (
            inside_brace_tag &&
            value == '}'
        ) {
            inside_brace_tag = 0;
            continue;
        }

        if (
            inside_angle_tag ||
            inside_brace_tag
        ) {
            continue;
        }

        if (
            value == '\\' &&
            (
                input[index + 1] == 'N' ||
                input[index + 1] == 'n'
            )
        ) {
            output[write_index++] = '\n';
            index++;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&amp;",
                5
            ) == 0
        ) {
            output[write_index++] = '&';
            index += 4;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&lt;",
                4
            ) == 0
        ) {
            output[write_index++] = '<';
            index += 3;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&gt;",
                4
            ) == 0
        ) {
            output[write_index++] = '>';
            index += 3;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&nbsp;",
                6
            ) == 0
        ) {
            output[write_index++] = ' ';
            index += 5;
            continue;
        }

        /*
         * Normalize common UTF-8 punctuation to characters supported
         * by the current UI font.
         */
        if (
            value == 0xE2 &&
            (unsigned char)input[index + 1] == 0x80
        ) {
            unsigned char third =
                (unsigned char)input[index + 2];

            if (
                third == 0x98 ||
                third == 0x99
            ) {
                output[write_index++] = '\'';
                index += 2;
                continue;
            }

            if (
                third == 0x9C ||
                third == 0x9D
            ) {
                output[write_index++] = '"';
                index += 2;
                continue;
            }

            if (
                third == 0x93 ||
                third == 0x94
            ) {
                output[write_index++] = '-';
                index += 2;
                continue;
            }

            if (third == 0xA6) {
                if (
                    write_index + 3 <
                    output_size
                ) {
                    output[write_index++] = '.';
                    output[write_index++] = '.';
                    output[write_index++] = '.';
                }

                index += 2;
                continue;
            }
        }

        if (
            value == 0xC2 &&
            (unsigned char)input[index + 1] == 0xA0
        ) {
            output[write_index++] = ' ';
            index++;
            continue;
        }

        if (value >= 128) {
            output[write_index++] = '?';

            while (
                input[index + 1] &&
                (
                    (
                        (unsigned char)
                        input[index + 1]
                    ) & 0xC0
                ) == 0x80
            ) {
                index++;
            }

            continue;
        }

        if (value == '\t') {
            value = ' ';
        }

        output[write_index++] =
            (char)value;
    }

    output[write_index] = 0;
    prospero_subtitle_trim(output);
}


static void prospero_subtitle_append_text(
    char *destination,
    size_t destination_size,
    const char *line
) {
    if (
        !destination ||
        !line ||
        destination_size == 0
    ) {
        return;
    }

    size_t used =
        strlen(destination);

    if (
        used > 0 &&
        used + 1 < destination_size
    ) {
        destination[used++] = '\n';
        destination[used] = 0;
    }

    if (used + 1 >= destination_size) {
        return;
    }

    snprintf(
        destination + used,
        destination_size - used,
        "%s",
        line
    );
}


static int prospero_subtitle_sidecar_path(
    const char *media_path,
    char *output,
    size_t output_size,
    const char *extension
) {
    if (
        !media_path ||
        !media_path[0] ||
        !output ||
        output_size == 0
    ) {
        return 0;
    }

    snprintf(
        output,
        output_size,
        "%s",
        media_path
    );

    char *last_slash =
        strrchr(output, '/');

    char *last_dot =
        strrchr(output, '.');

    if (
        last_dot &&
        (
            !last_slash ||
            last_dot > last_slash
        )
    ) {
        *last_dot = 0;
    }

    size_t used =
        strlen(output);

    if (
        used + strlen(extension) + 1 >
        output_size
    ) {
        return 0;
    }

    strcat(
        output,
        extension
    );

    return 1;
}


static int prospero_subtitle_load_file(
    const char *path
) {
    FILE *file =
        fopen(
            path,
            "rb"
        );

    if (!file) {
        return 0;
    }

    prospero_subtitle_count = 0;

    char line[1024];
    int first_line = 1;

    while (
        prospero_subtitle_count <
            PROSPERO_SUBTITLE_MAX_CUES &&
        fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        if (
            first_line &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF
        ) {
            memmove(
                line,
                line + 3,
                strlen(line + 3) + 1
            );
        }

        first_line = 0;
        prospero_subtitle_trim(line);

        if (!line[0]) {
            continue;
        }

        char timing_line[1024];

        if (strstr(line, "-->")) {
            snprintf(
                timing_line,
                sizeof(timing_line),
                "%s",
                line
            );
        } else {
            if (
                !fgets(
                    timing_line,
                    sizeof(timing_line),
                    file
                )
            ) {
                break;
            }

            prospero_subtitle_trim(
                timing_line
            );

            if (
                !strstr(
                    timing_line,
                    "-->"
                )
            ) {
                continue;
            }
        }

        double start_seconds = 0.0;
        double end_seconds = 0.0;

        if (
            !prospero_subtitle_parse_timing(
                timing_line,
                &start_seconds,
                &end_seconds
            )
        ) {
            continue;
        }

        ProsperoSubtitleCue *cue =
            &prospero_subtitle_cues[
                prospero_subtitle_count
            ];

        cue->start_seconds =
            start_seconds;

        cue->end_seconds =
            end_seconds;

        cue->text[0] = 0;

        while (
            fgets(
                line,
                sizeof(line),
                file
            )
        ) {
            prospero_subtitle_trim(line);

            if (!line[0]) {
                break;
            }

            char cleaned[
                PROSPERO_SUBTITLE_TEXT_SIZE
            ];

            prospero_subtitle_clean_line(
                line,
                cleaned,
                sizeof(cleaned)
            );

            if (cleaned[0]) {
                prospero_subtitle_append_text(
                    cue->text,
                    sizeof(cue->text),
                    cleaned
                );
            }
        }

        if (cue->text[0]) {
            prospero_subtitle_count++;
        }
    }

    fclose(file);

    if (prospero_subtitle_count <= 0) {
        return 0;
    }

    snprintf(
        prospero_subtitle_path,
        sizeof(prospero_subtitle_path),
        "%s",
        path
    );

    return prospero_subtitle_count;
}


int prospero_subtitle_load_for_media(
    const char *media_path
) {
    prospero_subtitle_clear();

    char sidecar_path[512];

    if (
        prospero_subtitle_sidecar_path(
            media_path,
            sidecar_path,
            sizeof(sidecar_path),
            ".srt"
        )
    ) {
        int count =
            prospero_subtitle_load_file(
                sidecar_path
            );

        if (count > 0) {
            char message[96];

            snprintf(
                message,
                sizeof(message),
                "Loaded %d cues",
                count
            );

            toast(
                "SUBTITLES",
                message
            );

            return count;
        }
    }

    if (
        prospero_subtitle_sidecar_path(
            media_path,
            sidecar_path,
            sizeof(sidecar_path),
            ".SRT"
        )
    ) {
        int count =
            prospero_subtitle_load_file(
                sidecar_path
            );

        if (count > 0) {
            char message[96];

            snprintf(
                message,
                sizeof(message),
                "Loaded %d cues",
                count
            );

            toast(
                "SUBTITLES",
                message
            );

            return count;
        }
    }

    prospero_subtitle_clear();
    return 0;
}


const ProsperoSubtitleCue *
prospero_subtitle_active_cue(
    double position
) {
    int low = 0;
    int high =
        prospero_subtitle_count - 1;

    while (low <= high) {
        int middle =
            low + (high - low) / 2;

        const ProsperoSubtitleCue *cue =
            &prospero_subtitle_cues[middle];

        if (
            position <
            cue->start_seconds
        ) {
            high = middle - 1;
        } else if (
            position >
            cue->end_seconds
        ) {
            low = middle + 1;
        } else {
            return cue;
        }
    }

    return NULL;
}

/* PROSPERO_SRT_MODULE_END */
/* PROSPERO_SUBTITLE_CONTROLS_START */

void prospero_subtitle_toggle(void) {
    int available =
        prospero_subtitle_count > 0 ||
        prospero_embedded_subtitle_stream_index >= 0;

    if (!available) {
        prospero_subtitle_enabled = 0;

        toast(
            "SUBTITLES",
            "NO SUPPORTED TEXT TRACK"
        );

        return;
    }

    prospero_subtitle_enabled =
        !prospero_subtitle_enabled;

    toast(
        "SUBTITLES",
        prospero_subtitle_enabled
            ? "ON"
            : "OFF"
    );

    controls_last_used_ms = now_ms();
}


static int prospero_subtitle_collect_tracks(
    int *tracks,
    int capacity
) {
    if (
        !tracks ||
        capacity <= 0 ||
        !play_fmt
    ) {
        return 0;
    }

    int count = 0;

    /*
     * -1 represents the matching external SRT.
     */
    if (
        prospero_subtitle_count > 0 &&
        count < capacity
    ) {
        tracks[count++] = -1;
    }

    for (
        unsigned int index = 0;
        index < play_fmt->nb_streams &&
        count < capacity;
        index++
    ) {
        AVStream *stream =
            play_fmt->streams[index];

        if (
            !stream ||
            !stream->codecpar ||
            stream->codecpar->codec_type !=
                AVMEDIA_TYPE_SUBTITLE
        ) {
            continue;
        }

        if (
            prospero_embedded_subtitle_supported(
                stream->codecpar->codec_id
            )
        ) {
            tracks[count++] = (int)index;
        }
    }

    return count;
}


static void prospero_subtitle_track_label(
    int track,
    char *output,
    size_t output_size
) {
    if (
        !output ||
        output_size == 0
    ) {
        return;
    }

    if (track < 0) {
        snprintf(
            output,
            output_size,
            "EXTERNAL SRT"
        );

        return;
    }

    if (
        !play_fmt ||
        track >= (int)play_fmt->nb_streams
    ) {
        snprintf(
            output,
            output_size,
            "EMBEDDED TEXT"
        );

        return;
    }

    AVStream *stream =
        play_fmt->streams[track];

    const char *language_name =
        "UNSPECIFIED";

    AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    if (
        language &&
        language->value &&
        language->value[0]
    ) {
        language_name = language->value;
    }

    const char *codec_name =
        avcodec_get_name(
            stream->codecpar->codec_id
        );

    snprintf(
        output,
        output_size,
        "%s / %s",
        language_name,
        codec_name
            ? codec_name
            : "TEXT"
    );
}


/*
 * Switch to `track` (-1 meaning the external SRT) and carry on from where we
 * are. Selecting a subtitle stream is a demuxer-open decision, so the file is
 * reopened and seeked back to the current position rather than switched in
 * place; the pause state survives so this does not silently start playing a
 * paused film.
 *
 * Split out of the D-pad cycler so the picker performs the identical switch.
 * Cycling made sense when a file had two tracks. Retail rips carry thirty-odd
 * and cycling through them one restart at a time is unusable, so the cycler
 * is now the fallback path and the picker is the one people will use.
 */
void prospero_subtitle_apply_track(int track)
{
    char label[128];

    if (
        !play_fmt ||
        !current_media_path[0]
    ) {
        return;
    }

    prospero_subtitle_track_label(
        track,
        label,
        sizeof(label)
    );

    double position =
        resume_base_offset_seconds +
        (
            audio_ctx &&
            audio_handle >= 1
                ? audio_clock_seconds
                : video_clock_seconds
        );

    if (position < 0.0) {
        position = 0.0;
    }

    int restore_paused = player_paused;

    char playback_path[512];

    snprintf(
        playback_path,
        sizeof(playback_path),
        "%s",
        current_media_path
    );

    prospero_subtitle_requested_stream = track;

    requested_resume_seek_pos = position;
    resume_base_offset_seconds = position;

    if (
        !start_video_playback(
            playback_path
        )
    ) {
        prospero_subtitle_requested_stream = -2;

        toast(
            "SUBTITLES",
            "TRACK CHANGE FAILED"
        );

        return;
    }

    if (restore_paused) {
        player_paused = 1;
    }

    prospero_subtitle_enabled = 1;

    toast(
        "SUBTITLES",
        label
    );

    controls_last_used_ms = now_ms();
}


static void prospero_subtitle_cycle_track(void) {
    if (
        screen != SCREEN_PLAYER ||
        !play_fmt ||
        !current_media_path[0]
    ) {
        return;
    }

    int tracks[64];

    int track_count =
        prospero_subtitle_collect_tracks(
            tracks,
            64
        );

    if (track_count <= 0) {
        prospero_subtitle_enabled = 0;

        toast(
            "SUBTITLES",
            "NO SUPPORTED TEXT TRACK"
        );

        return;
    }

    int current_track =
        prospero_subtitle_use_external
            ? -1
            : prospero_embedded_subtitle_stream_index;

    int current_slot = -1;

    for (
        int index = 0;
        index < track_count;
        index++
    ) {
        if (tracks[index] == current_track) {
            current_slot = index;
            break;
        }
    }

    int next_slot =
        current_slot < 0
            ? 0
            : (current_slot + 1) % track_count;

    int next_track =
        tracks[next_slot];

    char label[128];

    prospero_subtitle_track_label(
        next_track,
        label,
        sizeof(label)
    );

    if (
        track_count == 1 &&
        current_slot == 0
    ) {
        prospero_subtitle_enabled = 1;
        toast("SUBTITLES", label);
        return;
    }

    prospero_subtitle_apply_track(
        next_track
    );
}

/* PROSPERO_SUBTITLE_CONTROLS_END */
void prospero_subtitle_nudge_delay(int delta_ms)
{
    char msg[64];
    prospero_subtitle_delay_ms += delta_ms;
    if (prospero_subtitle_delay_ms < -5000)
        prospero_subtitle_delay_ms = -5000;
    if (prospero_subtitle_delay_ms > 5000)
        prospero_subtitle_delay_ms = 5000;

    if (prospero_subtitle_delay_ms == 0)
        snprintf(msg, sizeof(msg), "0 ms (sync)");
    else
        snprintf(
            msg,
            sizeof(msg),
            "%+d ms",
            prospero_subtitle_delay_ms);
    toast("SUB DELAY", msg);
    controls_last_used_ms = now_ms();
}
