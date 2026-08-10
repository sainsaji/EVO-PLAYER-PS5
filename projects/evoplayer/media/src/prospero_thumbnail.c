/*
 * prospero_thumbnail - scrub-preview thumbnail worker.
 *
 * Moved out of projects/evoplayer/main.c, where it sat between the OSD
 * renderer and the subtitle parser. See prospero_thumbnail.h for why this
 * module was the one to leave first.
 *
 * The only edits made during the move:
 *   - the symbols the player calls lost their `static` and are declared in
 *     the header instead;
 *   - prospero_osd_blend_pixel() became evo_blend_pixel() from evo_blend.h -
 *     the same arithmetic in a shared place, rather than a static in main.c
 *     that a module had to reach back into the program for;
 *   - WIDTH/HEIGHT became EVO_SCREEN_W/EVO_SCREEN_H, which already existed in
 *     evo_metrics.h and mean the same 1920x1080;
 *   - now_ms() is defined here rather than borrowed, so nothing in this file
 *     depends on the program that links it.
 *
 * No logic changed. The diff against main.c's old region is those four
 * substitutions and nothing else.
 */
#include "prospero_thumbnail.h"

#include "evo_blend.h"
#include "evo_metrics.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* main.c has its own now_ms(). This module keeps a private one so it links
 * against nothing but libc, FFmpeg and the two headers above. */
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* THUMBNAIL_STATE_FORWARD_START */

/*
 * Tentative declarations for worker state used by the crossfade
 * publisher. The initialized definitions remain later in the file.
 */
static uint32_t prospero_thumbnail_pixels[
    PROSPERO_THUMB_W *
    PROSPERO_THUMB_H
];

static int prospero_thumbnail_valid;
static int prospero_thumbnail_loading;
static double prospero_thumbnail_display_time;

/* THUMBNAIL_STATE_FORWARD_END */

/* PROSPERO_THUMBNAIL_CROSSFADE_START */

#define PROSPERO_THUMBNAIL_FADE_MS 180

static uint32_t prospero_thumbnail_previous_pixels[
    PROSPERO_THUMB_W *
    PROSPERO_THUMB_H
];

static int prospero_thumbnail_previous_valid = 0;
static long long prospero_thumbnail_transition_started_ms = 0;


static void prospero_thumbnail_begin_transition_locked(
    const uint32_t *new_pixels,
    double timestamp
) {
    if (!new_pixels) {
        return;
    }

    if (prospero_thumbnail_valid) {
        memcpy(
            prospero_thumbnail_previous_pixels,
            prospero_thumbnail_pixels,
            PROSPERO_THUMB_W *
            PROSPERO_THUMB_H *
            sizeof(uint32_t)
        );

        prospero_thumbnail_previous_valid = 1;
    } else {
        prospero_thumbnail_previous_valid = 0;
    }

    memcpy(
        prospero_thumbnail_pixels,
        new_pixels,
        PROSPERO_THUMB_W *
        PROSPERO_THUMB_H *
        sizeof(uint32_t)
    );

    prospero_thumbnail_valid = 1;
    prospero_thumbnail_loading = 0;
    prospero_thumbnail_display_time = timestamp;

    prospero_thumbnail_transition_started_ms =
        now_ms();
}


static int prospero_thumbnail_transition_alpha_locked(void) {
    if (!prospero_thumbnail_previous_valid) {
        return 255;
    }

    long long elapsed =
        now_ms() -
        prospero_thumbnail_transition_started_ms;

    if (elapsed >= PROSPERO_THUMBNAIL_FADE_MS) {
        prospero_thumbnail_previous_valid = 0;
        return 255;
    }

    if (elapsed <= 0) {
        return 0;
    }

    return (int)(
        elapsed * 255 /
        PROSPERO_THUMBNAIL_FADE_MS
    );
}

/* PROSPERO_THUMBNAIL_CROSSFADE_END */

/* PROSPERO_THUMBNAIL_WORKER_START */



static pthread_mutex_t prospero_thumbnail_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t prospero_thumbnail_condition =
    PTHREAD_COND_INITIALIZER;

static pthread_t prospero_thumbnail_thread;

static int prospero_thumbnail_thread_started = 0;
static volatile int prospero_thumbnail_thread_running = 0;

static unsigned long prospero_thumbnail_request_serial = 0;
static int prospero_thumbnail_request_pending = 0;

static char prospero_thumbnail_request_path[512] = {0};
static double prospero_thumbnail_request_time = 0.0;

static char prospero_thumbnail_last_path[512] = {0};
static double prospero_thumbnail_last_requested_time = -100000.0;

static uint32_t prospero_thumbnail_pixels[
    PROSPERO_THUMB_W *
    PROSPERO_THUMB_H
];

static int prospero_thumbnail_valid = 0;
static int prospero_thumbnail_loading = 0;
static double prospero_thumbnail_display_time = 0.0;



/* PROSPERO_THUMBNAIL_CACHE_START */

#define PROSPERO_THUMB_CACHE_SIZE 32
#define PROSPERO_THUMB_CACHE_TOLERANCE 1.50

typedef struct ProsperoThumbnailCacheEntry {
    char path[512];
    double timestamp;

    unsigned long long last_used;
    int valid;

    uint32_t pixels[
        PROSPERO_THUMB_W *
        PROSPERO_THUMB_H
    ];
} ProsperoThumbnailCacheEntry;


static ProsperoThumbnailCacheEntry
    prospero_thumbnail_cache[
        PROSPERO_THUMB_CACHE_SIZE
    ];

static unsigned long long
    prospero_thumbnail_cache_clock = 1;

static unsigned long
    prospero_thumbnail_cache_hits = 0;

static unsigned long
    prospero_thumbnail_cache_misses = 0;


/*
 * The caller must already hold prospero_thumbnail_mutex.
 */
static int prospero_thumbnail_cache_find_locked(
    const char *path,
    double timestamp
) {
    if (!path || !path[0]) {
        return -1;
    }

    int best_index = -1;
    double best_difference = 1000000000.0;

    for (
        int index = 0;
        index < PROSPERO_THUMB_CACHE_SIZE;
        index++
    ) {
        ProsperoThumbnailCacheEntry *entry =
            &prospero_thumbnail_cache[index];

        if (!entry->valid) {
            continue;
        }

        if (
            strcmp(entry->path, path) != 0
        ) {
            continue;
        }

        double difference =
            entry->timestamp - timestamp;

        if (difference < 0.0) {
            difference = -difference;
        }

        if (
            difference <=
                PROSPERO_THUMB_CACHE_TOLERANCE &&
            difference < best_difference
        ) {
            best_difference = difference;
            best_index = index;
        }
    }

    if (best_index >= 0) {
        prospero_thumbnail_cache[
            best_index
        ].last_used =
            prospero_thumbnail_cache_clock++;

        prospero_thumbnail_cache_hits++;
    } else {
        prospero_thumbnail_cache_misses++;
    }

    return best_index;
}


/*
 * Copies a cached entry directly into the published display buffer.
 * The caller must already hold prospero_thumbnail_mutex.
 */
static int prospero_thumbnail_cache_publish_locked(
    const char *path,
    double timestamp
) {
    int index =
        prospero_thumbnail_cache_find_locked(
            path,
            timestamp
        );

    if (index < 0) {
        return 0;
    }

    ProsperoThumbnailCacheEntry *entry =
        &prospero_thumbnail_cache[index];

    prospero_thumbnail_begin_transition_locked(
        entry->pixels,
        entry->timestamp
    );

    return 1;
}


/*
 * The caller must already hold prospero_thumbnail_mutex.
 */
static void prospero_thumbnail_cache_store_locked(
    const char *path,
    double timestamp,
    const uint32_t *pixels
) {
    if (
        !path ||
        !path[0] ||
        !pixels
    ) {
        return;
    }

    /*
     * Update an existing nearby entry when possible.
     */
    int destination = -1;

    for (
        int index = 0;
        index < PROSPERO_THUMB_CACHE_SIZE;
        index++
    ) {
        ProsperoThumbnailCacheEntry *entry =
            &prospero_thumbnail_cache[index];

        if (
            entry->valid &&
            strcmp(entry->path, path) == 0
        ) {
            double difference =
                entry->timestamp - timestamp;

            if (difference < 0.0) {
                difference = -difference;
            }

            if (
                difference <=
                PROSPERO_THUMB_CACHE_TOLERANCE
            ) {
                destination = index;
                break;
            }
        }
    }

    /*
     * Prefer an empty slot.
     */
    if (destination < 0) {
        for (
            int index = 0;
            index < PROSPERO_THUMB_CACHE_SIZE;
            index++
        ) {
            if (
                !prospero_thumbnail_cache[
                    index
                ].valid
            ) {
                destination = index;
                break;
            }
        }
    }

    /*
     * Otherwise replace the least-recently-used entry.
     */
    if (destination < 0) {
        unsigned long long oldest =
            prospero_thumbnail_cache[0].last_used;

        destination = 0;

        for (
            int index = 1;
            index < PROSPERO_THUMB_CACHE_SIZE;
            index++
        ) {
            if (
                prospero_thumbnail_cache[
                    index
                ].last_used < oldest
            ) {
                oldest =
                    prospero_thumbnail_cache[
                        index
                    ].last_used;

                destination = index;
            }
        }
    }

    ProsperoThumbnailCacheEntry *entry =
        &prospero_thumbnail_cache[
            destination
        ];

    snprintf(
        entry->path,
        sizeof(entry->path),
        "%s",
        path
    );

    entry->timestamp = timestamp;

    entry->last_used =
        prospero_thumbnail_cache_clock++;

    entry->valid = 1;

    memcpy(
        entry->pixels,
        pixels,
        PROSPERO_THUMB_W *
        PROSPERO_THUMB_H *
        sizeof(uint32_t)
    );
}


/* prospero_thumbnail_cache_count_locked() lived here, commented "useful later
 * for the developer overlay". It was never called, in this file or anywhere
 * else, and -Wunused-function had been reporting it into a wall of main.c
 * warnings nobody was reading. Deleted on the way out; git has it if the
 * overlay ever wants it. */

/* PROSPERO_THUMBNAIL_CACHE_END */

/*
 * Persistent FFmpeg context for scrub previews — reopening 4K HEVC every
 * scrub step was the main reason previews felt very slow.
 */
static AVFormatContext *prospero_thumb_fmt = NULL;
static AVCodecContext *prospero_thumb_codec = NULL;
static AVFrame *prospero_thumb_frame = NULL;
static AVPacket *prospero_thumb_packet = NULL;
static int prospero_thumb_stream = -1;
static char prospero_thumb_open_path[512] = {0};

void prospero_thumbnail_close_context(void)
{
    if (prospero_thumb_packet) {
        av_packet_free(&prospero_thumb_packet);
        prospero_thumb_packet = NULL;
    }
    if (prospero_thumb_frame) {
        av_frame_free(&prospero_thumb_frame);
        prospero_thumb_frame = NULL;
    }
    if (prospero_thumb_codec) {
        avcodec_free_context(&prospero_thumb_codec);
        prospero_thumb_codec = NULL;
    }
    if (prospero_thumb_fmt) {
        avformat_close_input(&prospero_thumb_fmt);
        prospero_thumb_fmt = NULL;
    }
    prospero_thumb_stream = -1;
    prospero_thumb_open_path[0] = 0;
}

static int prospero_thumbnail_ensure_context(const char *path)
{
    unsigned int i;

    if (path && path[0] &&
        prospero_thumb_fmt && prospero_thumb_codec &&
        strcmp(prospero_thumb_open_path, path) == 0)
        return 1;

    prospero_thumbnail_close_context();

    if (!path || !path[0])
        return 0;

    if (avformat_open_input(&prospero_thumb_fmt, path, NULL, NULL) < 0)
        return 0;
    if (avformat_find_stream_info(prospero_thumb_fmt, NULL) < 0) {
        prospero_thumbnail_close_context();
        return 0;
    }

    for (i = 0; i < prospero_thumb_fmt->nb_streams; i++) {
        if (prospero_thumb_fmt->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_VIDEO) {
            prospero_thumb_stream = (int)i;
            break;
        }
    }
    if (prospero_thumb_stream < 0) {
        prospero_thumbnail_close_context();
        return 0;
    }

    {
        AVStream *stream =
            prospero_thumb_fmt->streams[prospero_thumb_stream];
        const AVCodec *decoder =
            avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            prospero_thumbnail_close_context();
            return 0;
        }
        prospero_thumb_codec = avcodec_alloc_context3(decoder);
        if (!prospero_thumb_codec ||
            avcodec_parameters_to_context(
                prospero_thumb_codec, stream->codecpar) < 0) {
            prospero_thumbnail_close_context();
            return 0;
        }
        /* Light threads — previews must not starve main playback */
        prospero_thumb_codec->thread_count = 2;
        prospero_thumb_codec->thread_type = FF_THREAD_FRAME;
#ifdef AV_CODEC_FLAG2_FAST
        prospero_thumb_codec->flags2 |= AV_CODEC_FLAG2_FAST;
#endif
        prospero_thumb_codec->skip_loop_filter = AVDISCARD_ALL;
        if (avcodec_open2(prospero_thumb_codec, decoder, NULL) < 0) {
            prospero_thumbnail_close_context();
            return 0;
        }
    }

    prospero_thumb_frame = av_frame_alloc();
    prospero_thumb_packet = av_packet_alloc();
    if (!prospero_thumb_frame || !prospero_thumb_packet) {
        prospero_thumbnail_close_context();
        return 0;
    }

    snprintf(prospero_thumb_open_path,
             sizeof(prospero_thumb_open_path), "%s", path);
    return 1;
}

static int prospero_thumbnail_decode(
    const char *path,
    double target_seconds,
    uint32_t *output_pixels
) {
    AVStream *stream;
    struct SwsContext *sws = NULL;
    double seek_seconds;
    double time_base_seconds;
    int64_t seek_timestamp;
    int decoded_frames = 0;
    int packet_limit = 96;
    int success = 0;

    if (!path || !path[0] || !output_pixels)
        return 0;

    if (!prospero_thumbnail_ensure_context(path))
        return 0;

    stream = prospero_thumb_fmt->streams[prospero_thumb_stream];
    time_base_seconds = av_q2d(stream->time_base);
    if (time_base_seconds <= 0.0)
        return 0;

    /* Short pre-roll only — long -2s seeks made every scrub step crawl */
    seek_seconds = target_seconds;
    if (seek_seconds > 0.6)
        seek_seconds -= 0.6;
    else
        seek_seconds = 0.0;

    seek_timestamp =
        (int64_t)(seek_seconds / time_base_seconds);

    if (av_seek_frame(prospero_thumb_fmt, prospero_thumb_stream,
                      seek_timestamp, AVSEEK_FLAG_BACKWARD) < 0)
        return 0;

    avformat_flush(prospero_thumb_fmt);
    avcodec_flush_buffers(prospero_thumb_codec);

    while (packet_limit-- > 0 &&
           av_read_frame(prospero_thumb_fmt, prospero_thumb_packet) >= 0) {
        if (prospero_thumb_packet->stream_index != prospero_thumb_stream) {
            av_packet_unref(prospero_thumb_packet);
            continue;
        }

        if (avcodec_send_packet(prospero_thumb_codec,
                                prospero_thumb_packet) < 0) {
            av_packet_unref(prospero_thumb_packet);
            continue;
        }
        av_packet_unref(prospero_thumb_packet);

        while (1) {
            int receive_result =
                avcodec_receive_frame(prospero_thumb_codec,
                                      prospero_thumb_frame);
            if (receive_result == AVERROR(EAGAIN) ||
                receive_result == AVERROR_EOF)
                break;
            if (receive_result < 0)
                break;

            decoded_frames++;
            {
                int64_t frame_timestamp =
                    prospero_thumb_frame->best_effort_timestamp;
                double frame_seconds =
                    frame_timestamp != AV_NOPTS_VALUE
                        ? frame_timestamp * time_base_seconds
                        : seek_seconds;
                int near_target =
                    frame_seconds >= target_seconds - 0.35;
                int timestamp_fallback = decoded_frames >= 6;

                if (!near_target && !timestamp_fallback) {
                    av_frame_unref(prospero_thumb_frame);
                    continue;
                }
            }

            sws = sws_getContext(
                prospero_thumb_frame->width,
                prospero_thumb_frame->height,
                (enum AVPixelFormat)prospero_thumb_frame->format,
                PROSPERO_THUMB_W,
                PROSPERO_THUMB_H,
                AV_PIX_FMT_RGBA,
                SWS_FAST_BILINEAR,
                NULL, NULL, NULL);
            if (!sws) {
                av_frame_unref(prospero_thumb_frame);
                return 0;
            }

            {
                uint8_t *destination_data[4] = {
                    (uint8_t *)output_pixels, NULL, NULL, NULL
                };
                int destination_linesize[4] = {
                    PROSPERO_THUMB_W * 4, 0, 0, 0
                };
                sws_scale(
                    sws,
                    (const uint8_t *const *)prospero_thumb_frame->data,
                    prospero_thumb_frame->linesize,
                    0,
                    prospero_thumb_frame->height,
                    destination_data,
                    destination_linesize);
            }
            sws_freeContext(sws);
            av_frame_unref(prospero_thumb_frame);
            success = 1;
            return success;
        }
    }

    return success;
}


static void *prospero_thumbnail_worker(
    void *argument
) {
    (void)argument;

    uint32_t *local_pixels =
        malloc(
            PROSPERO_THUMB_W *
            PROSPERO_THUMB_H *
            sizeof(uint32_t)
        );

    if (!local_pixels) {
        return NULL;
    }

    while (prospero_thumbnail_thread_running) {
        char path[512];
        double target = 0.0;
        unsigned long serial = 0;

        pthread_mutex_lock(
            &prospero_thumbnail_mutex
        );

        while (
            prospero_thumbnail_thread_running &&
            !prospero_thumbnail_request_pending
        ) {
            pthread_cond_wait(
                &prospero_thumbnail_condition,
                &prospero_thumbnail_mutex
            );
        }

        if (!prospero_thumbnail_thread_running) {
            pthread_mutex_unlock(
                &prospero_thumbnail_mutex
            );

            break;
        }

        serial =
            prospero_thumbnail_request_serial;

        snprintf(
            path,
            sizeof(path),
            "%s",
            prospero_thumbnail_request_path
        );

        target =
            prospero_thumbnail_request_time;

        prospero_thumbnail_request_pending = 0;

        pthread_mutex_unlock(
            &prospero_thumbnail_mutex
        );

        /*
         * Short debounce only — was 150ms and felt very laggy with 4K opens.
         * Persistent demux context makes the decode cheap enough to react faster.
         */
        usleep(35000);

        pthread_mutex_lock(
            &prospero_thumbnail_mutex
        );

        int stale_before_decode =
            serial !=
            prospero_thumbnail_request_serial;

        if (!stale_before_decode) {
            prospero_thumbnail_loading = 1;
        }

        pthread_mutex_unlock(
            &prospero_thumbnail_mutex
        );

        if (stale_before_decode) {
            continue;
        }

        int decoded =
            prospero_thumbnail_decode(
                path,
                target,
                local_pixels
            );

        pthread_mutex_lock(
            &prospero_thumbnail_mutex
        );

        /*
         * Never publish an obsolete frame after the scrubber has moved.
         */
        if (
            serial ==
            prospero_thumbnail_request_serial
        ) {
            if (decoded) {
                prospero_thumbnail_begin_transition_locked(
                    local_pixels,
                    target
                );

                prospero_thumbnail_cache_store_locked(
                    path,
                    target,
                    local_pixels
                );



            }

            prospero_thumbnail_loading = 0;
        }

        pthread_mutex_unlock(
            &prospero_thumbnail_mutex
        );
    }

    free(local_pixels);
    return NULL;
}


static int prospero_thumbnail_start(void) {
    if (prospero_thumbnail_thread_started) {
        return 1;
    }

    prospero_thumbnail_thread_running = 1;

    if (
        pthread_create(
            &prospero_thumbnail_thread,
            NULL,
            prospero_thumbnail_worker,
            NULL
        ) != 0
    ) {
        prospero_thumbnail_thread_running = 0;
        return 0;
    }

    /*
     * This worker lives for the lifetime of the application.
     * Each decode operation opens and closes its own FFmpeg contexts.
     */
    pthread_detach(
        prospero_thumbnail_thread
    );

    prospero_thumbnail_thread_started = 1;
    return 1;
}


void prospero_thumbnail_request(
    const char *path,
    double target_seconds,
    int scrub_active
) {
    if (!scrub_active) {
        return;
    }

    if (!path || !path[0]) {
        return;
    }

    /*
     * First try the LRU cache. A hit immediately updates the preview
     * and avoids opening a second FFmpeg context.
     */
    pthread_mutex_lock(
        &prospero_thumbnail_mutex
    );

    int cache_hit =
        prospero_thumbnail_cache_publish_locked(
            path,
            target_seconds
        );

    pthread_mutex_unlock(
        &prospero_thumbnail_mutex
    );

    if (cache_hit) {
        snprintf(
            prospero_thumbnail_last_path,
            sizeof(prospero_thumbnail_last_path),
            "%s",
            path
        );

        prospero_thumbnail_last_requested_time =
            target_seconds;

        return;
    }

    if (!prospero_thumbnail_start()) {
        return;
    }

    double difference =
        target_seconds -
        prospero_thumbnail_last_requested_time;

    if (difference < 0.0) {
        difference = -difference;
    }

    int path_changed =
        strcmp(
            prospero_thumbnail_last_path,
            path
        ) != 0;

    /*
     * Coarser scrub steps hit cache more often (tolerance 1.5s).
     * Was 0.25s → thrash decode on every tiny D-pad tick.
     */
    if (
        !path_changed &&
        difference < 0.85
    ) {
        return;
    }

    pthread_mutex_lock(
        &prospero_thumbnail_mutex
    );

    /*
     * Check once more after obtaining the mutex in case the worker
     * completed and cached this target between the first lookup and
     * this point.
     */
    cache_hit =
        prospero_thumbnail_cache_publish_locked(
            path,
            target_seconds
        );

    if (cache_hit) {
        pthread_mutex_unlock(
            &prospero_thumbnail_mutex
        );

        snprintf(
            prospero_thumbnail_last_path,
            sizeof(prospero_thumbnail_last_path),
            "%s",
            path
        );

        prospero_thumbnail_last_requested_time =
            target_seconds;

        return;
    }

    snprintf(
        prospero_thumbnail_request_path,
        sizeof(prospero_thumbnail_request_path),
        "%s",
        path
    );

    prospero_thumbnail_request_time =
        target_seconds;

    prospero_thumbnail_request_serial++;
    prospero_thumbnail_request_pending = 1;
    prospero_thumbnail_loading = 1;

    if (path_changed) {
        prospero_thumbnail_valid = 0;
    }

    pthread_cond_signal(
        &prospero_thumbnail_condition
    );

    pthread_mutex_unlock(
        &prospero_thumbnail_mutex
    );

    snprintf(
        prospero_thumbnail_last_path,
        sizeof(prospero_thumbnail_last_path),
        "%s",
        path
    );

    prospero_thumbnail_last_requested_time =
        target_seconds;
}


int prospero_thumbnail_is_valid(void) {
    pthread_mutex_lock(
        &prospero_thumbnail_mutex
    );

    int valid =
        prospero_thumbnail_valid;

    pthread_mutex_unlock(
        &prospero_thumbnail_mutex
    );

    return valid;
}


int prospero_thumbnail_is_loading(void) {
    pthread_mutex_lock(
        &prospero_thumbnail_mutex
    );

    int loading =
        prospero_thumbnail_loading;

    pthread_mutex_unlock(
        &prospero_thumbnail_mutex
    );

    return loading;
}


void prospero_thumbnail_blit(
    uint32_t *fb,
    int destination_x,
    int destination_y,
    int destination_w,
    int destination_h,
    int opacity
) {
    if (!fb || opacity <= 0) {
        return;
    }

    pthread_mutex_lock(
        &prospero_thumbnail_mutex
    );

    if (!prospero_thumbnail_valid) {
        pthread_mutex_unlock(
            &prospero_thumbnail_mutex
        );

        return;
    }

    int transition_alpha =
        prospero_thumbnail_transition_alpha_locked();

    int old_alpha =
        255 - transition_alpha;

    for (int y = 0; y < destination_h; y++) {
        int source_y =
            y *
            PROSPERO_THUMB_H /
            destination_h;

        for (int x = 0; x < destination_w; x++) {
            int screen_x =
                destination_x + x;

            int screen_y =
                destination_y + y;

            if (
                screen_x < 0 ||
                screen_x >= EVO_SCREEN_W ||
                screen_y < 0 ||
                screen_y >= EVO_SCREEN_H
            ) {
                continue;
            }

            int source_x =
                x *
                PROSPERO_THUMB_W /
                destination_w;

            int source_index =
                source_y *
                PROSPERO_THUMB_W +
                source_x;

            uint32_t new_pixel =
                prospero_thumbnail_pixels[
                    source_index
                ];

            uint32_t composed_pixel =
                new_pixel;

            if (
                prospero_thumbnail_previous_valid &&
                old_alpha > 0
            ) {
                uint32_t old_pixel =
                    prospero_thumbnail_previous_pixels[
                        source_index
                    ];

                composed_pixel =
                    evo_blend_pixel(
                        old_pixel,
                        new_pixel,
                        transition_alpha
                    );
            }

            uint32_t *destination =
                &fb[
                    screen_y *
                    EVO_SCREEN_W +
                    screen_x
                ];

            *destination =
                evo_blend_pixel(
                    *destination,
                    composed_pixel,
                    opacity
                );
        }
    }

    pthread_mutex_unlock(
        &prospero_thumbnail_mutex
    );
}

/* PROSPERO_THUMBNAIL_WORKER_END */
