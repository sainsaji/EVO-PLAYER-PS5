/*
 * evo_controller_speaker.c — DualSense controller speaker routing for in-movie phone audio.
 */
#include "evo_controller_speaker.h"
#include "evo_subtitle.h"
#include "evo_boot_log.h"
#include "evo_thread.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>

extern int g_ps5_user_id;

#define SCE_AUDIO_OUT_PORT_TYPE_PADSPK        4
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO   0
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO 1

int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutOutput(int handle, const void *ptr);
int sceAudioOutClose(int handle);

#define PAD_MAX_GRAIN    2048
#define PAD_RING_BLOCKS  64
#define HANGOVER_BLOCKS  10   /* ~420 ms hangover at 2048 frames (42.66 ms per block) */

static int s_enabled = 0;
static int s_user_id = 0;
static int s_pad_handle = -1;
static int s_pad_grain = 256;
static int s_is_stereo_port = 0;

static volatile int s_phone_active = 0;
static int s_hangover = 0;

static pthread_t s_thread;
static volatile int s_thread_running = 0;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;

static int16_t s_ring[PAD_RING_BLOCKS][PAD_MAX_GRAIN * 2];
static volatile int s_ring_head = 0;
static volatile int s_ring_tail = 0;
static volatile int s_ring_count = 0;

/* Biquad filter state for telephone DSP */
typedef struct {
    float b0, b1, b2, a1, a2;
    float s1, s2;
} biquad_t;

static biquad_t s_hp_filter;
static biquad_t s_lp_filter;
static int s_filters_initialized = 0;

static inline float biquad_process(biquad_t *f, float in) {
    float out = f->b0 * in + f->s1;
    f->s1 = f->b1 * in - f->a1 * out + f->s2;
    f->s2 = f->b2 * in - f->a2 * out;
    return out;
}

/* RBJ biquads at fs = 48 kHz, Q = 0.7071: high-pass 350 Hz, low-pass 3400 Hz.
 * The coefficients are precomputed on the host because sinf/cosf are not
 * calls EVO made anywhere else, and a libc function the app module's shim
 * lacks binds to NULL and crashes at address 0 on first use. */
static void init_telephone_filters(void) {
    static const biquad_t hp = { 9.681226875e-01f, -1.936245375e+00f, 9.681226875e-01f,
                                 -1.935228973e+00f, 9.372617774e-01f, 0.0f, 0.0f };
    static const biquad_t lp = { 3.734023477e-02f, 7.468046954e-02f, 3.734023477e-02f,
                                 -1.383887278e+00f, 5.332482170e-01f, 0.0f, 0.0f };
    s_hp_filter = hp;
    s_lp_filter = lp;
    s_filters_initialized = 1;
}

/* ASCII case-insensitive substring search (libc strcasestr is not imported on PS5) */
static int evo_ascii_has_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return 0;
    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; ++h) {
        size_t i = 0;
        while (i < nlen && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

/* True when `word` appears in buf (already lower-case) as a whole word, so
 * "phone" matches "[on phone]" but not "microphone" or "headphones". */
static int has_word(const char *buf, const char *word) {
    size_t wl = strlen(word);
    for (const char *h = strstr(buf, word); h; h = strstr(h + 1, word)) {
        int left_ok = (h == buf) || !isalpha((unsigned char)h[-1]);
        char after = h[wl];
        int right_ok = !isalpha((unsigned char)after) ||
                       (after == 's' && !isalpha((unsigned char)h[wl + 1]));
        if (left_ok && right_ok) return 1;
    }
    return 0;
}

static int descriptor_has_phone(const char *tag, size_t len) {
    /* Only words that mean a phone call. "cell", "receiver", "filtered" and
     * "distorted" used to be here: "[cell door slams]" or "[distorted
     * screaming]" then ducked the whole main mix. */
    static const char * const kw[] = {
        "phone", "telephone", "cellphone", "cell phone", "caller",
        "answering machine", "voicemail", "speakerphone"
    };
    char buf[128];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    for (size_t i = 0; i < len; ++i) {
        buf[i] = (char)tolower((unsigned char)tag[i]);
    }
    buf[len] = '\0';

    for (size_t k = 0; k < sizeof(kw) / sizeof(kw[0]); ++k) {
        if (has_word(buf, kw[k])) {
            return 1;
        }
    }
    return 0;
}

static int subtitle_text_matches_phone(const char *text) {
    if (!text || !text[0]) return 0;

    /* 1. Check for bracketed descriptors: [on phone], (over telephone), etc. */
    for (const char *p = text; *p; ++p) {
        if (*p == '[' || *p == '(') {
            char close = (*p == '[') ? ']' : ')';
            const char *start = p + 1;
            const char *end = strchr(start, close);
            if (end && end > start) {
                if (descriptor_has_phone(start, (size_t)(end - start))) {
                    return 1;
                }
                p = end;
            }
        }
    }

    /* 2. Check for speaker prefixes: "Caller:", "On phone:", "Phone:" */
    static const char * const prefixes[] = {
        "caller:", "on phone:", "over phone:", "phone:", "telephone:"
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        if (evo_ascii_has_ci(text, prefixes[i])) {
            return 1;
        }
    }

    return 0;
}

static int check_phone_cues(double clock_seconds) {
    /* 1. Check external SRT active cue */
    const ProsperoSubtitleCue *cue = prospero_subtitle_active_cue(clock_seconds);
    if (cue && cue->text[0]) {
        if (subtitle_text_matches_phone(cue->text))
            return 1;
    }

    /* 2. Check embedded subtitle text */
    char emb_buf[512];
    emb_buf[0] = '\0';
    if (prospero_embedded_subtitle_text_at(clock_seconds, emb_buf, sizeof(emb_buf)) > 0) {
        if (subtitle_text_matches_phone(emb_buf))
            return 1;
    }

    return 0;
}

static void *controller_speaker_worker(void *arg) {
    (void)arg;
    int16_t out_buf[PAD_MAX_GRAIN * 2];

    while (s_thread_running) {
        pthread_mutex_lock(&s_mutex);
        while (s_ring_count == 0 && s_thread_running) {
            pthread_cond_wait(&s_cond, &s_mutex);
        }

        if (!s_thread_running) {
            pthread_mutex_unlock(&s_mutex);
            break;
        }

        size_t samples_to_send = (size_t)s_pad_grain * (s_is_stereo_port ? 2 : 1);
        memcpy(out_buf, s_ring[s_ring_tail], samples_to_send * sizeof(int16_t));
        s_ring_tail = (s_ring_tail + 1) % PAD_RING_BLOCKS;
        s_ring_count--;
        pthread_mutex_unlock(&s_mutex);

        if (s_pad_handle >= 0) {
            sceAudioOutOutput(s_pad_handle, out_buf);
        }
    }

    if (s_pad_handle >= 0) {
        sceAudioOutClose(s_pad_handle);
        s_pad_handle = -1;
    }
    return NULL;
}

void evo_controller_speaker_init(int user_id) {
    s_user_id = user_id;
    if (!s_filters_initialized) {
        init_telephone_filters();
    }
}

void evo_controller_speaker_set_enabled(int enabled) {
    s_enabled = enabled ? 1 : 0;
}

int evo_controller_speaker_is_enabled(void) {
    return s_enabled;
}

void evo_controller_speaker_start(int enabled) {
    s_enabled = enabled ? 1 : 0;
    if (!s_enabled) return;

    if (!s_filters_initialized) {
        init_telephone_filters();
    }

    int uid = s_user_id > 0 ? s_user_id : (g_ps5_user_id > 0 ? g_ps5_user_id : 0xFF);

    /* Open PADSPK port: try 2048 first to match main audio grain 1:1, then 256 */
    s_pad_grain = 2048;
    s_is_stereo_port = 0;
    s_pad_handle = sceAudioOutOpen(uid, SCE_AUDIO_OUT_PORT_TYPE_PADSPK, 0,
                                   s_pad_grain, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO);
    if (s_pad_handle < 0) {
        s_pad_handle = sceAudioOutOpen(uid, SCE_AUDIO_OUT_PORT_TYPE_PADSPK, 0,
                                       s_pad_grain, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        if (s_pad_handle >= 0) s_is_stereo_port = 1;
    }

    if (s_pad_handle < 0) {
        /* Fallback to 256 grain */
        s_pad_grain = 256;
        s_pad_handle = sceAudioOutOpen(uid, SCE_AUDIO_OUT_PORT_TYPE_PADSPK, 0,
                                       s_pad_grain, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO);
        if (s_pad_handle < 0) {
            s_pad_handle = sceAudioOutOpen(uid, SCE_AUDIO_OUT_PORT_TYPE_PADSPK, 0,
                                           s_pad_grain, 48000, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
            if (s_pad_handle >= 0) s_is_stereo_port = 1;
        }
    }

    if (s_pad_handle >= 0) {
        evo_boot_log("controller_speaker: opened PADSPK port handle=%d (grain=%d, stereo=%d, uid=0x%x)",
                     s_pad_handle, s_pad_grain, s_is_stereo_port, uid);
    } else {
        evo_boot_log("controller_speaker: PADSPK port refused (code=%d, uid=0x%x)",
                     s_pad_handle, uid);
    }

    s_ring_head = 0;
    s_ring_tail = 0;
    s_ring_count = 0;
    s_phone_active = 0;
    s_hangover = 0;
    s_thread_running = 1;
    /* The console's default pthread stack is too small for a thread that
     * calls into the audio library with an 8 KB buffer on its stack. */
    if (evo_thread_create(&s_thread, controller_speaker_worker, NULL) != 0) {
        s_thread_running = 0;
        if (s_pad_handle >= 0) {
            sceAudioOutClose(s_pad_handle);
            s_pad_handle = -1;
        }
    }
}

void evo_controller_speaker_stop(void) {
    if (s_thread_running) {
        pthread_mutex_lock(&s_mutex);
        s_thread_running = 0;
        pthread_cond_signal(&s_cond);
        pthread_mutex_unlock(&s_mutex);
        pthread_join(s_thread, NULL);
    }
    s_phone_active = 0;
    s_hangover = 0;
}

void evo_controller_speaker_flush(void) {
    pthread_mutex_lock(&s_mutex);
    s_ring_head = 0;
    s_ring_tail = 0;
    s_ring_count = 0;
    s_phone_active = 0;
    s_hangover = 0;
    pthread_mutex_unlock(&s_mutex);
}

int evo_controller_speaker_is_phone_active(void) {
    return s_phone_active;
}

void evo_controller_speaker_feed(const int16_t *samples, size_t frames, int channels, double clock_seconds) {
    if (!s_enabled || !s_thread_running || s_pad_handle < 0 || !samples || frames == 0 || channels <= 0) {
        return;
    }

    /* 1. Detect if phone dialogue cue is active */
    int detected = check_phone_cues(clock_seconds);
    int prev_active = s_phone_active;
    if (detected) {
        s_hangover = HANGOVER_BLOCKS;
        s_phone_active = 1;
    } else if (s_hangover > 0) {
        s_hangover--;
        s_phone_active = 1;
    } else {
        s_phone_active = 0;
    }

    if (!s_phone_active) {
        return;
    }

    if (!prev_active && s_phone_active) {
        /* Reset biquad states on cue start to avoid stale impulse click */
        s_hp_filter.s1 = s_hp_filter.s2 = 0.0f;
        s_lp_filter.s1 = s_lp_filter.s2 = 0.0f;
    }

    /* Process all frames in slices of s_pad_grain */
    int16_t grain_buf[PAD_MAX_GRAIN * 2];

    for (size_t chunk_start = 0; chunk_start < frames; chunk_start += s_pad_grain) {
        size_t chunk_frames = frames - chunk_start;
        if (chunk_frames > (size_t)s_pad_grain) {
            chunk_frames = s_pad_grain;
        }

        for (size_t i = 0; i < chunk_frames; ++i) {
            size_t src_idx = chunk_start + i;
            int32_t mono_raw;
            if (channels >= 6) {
                /* Surround source: channel 2 is Front Center dialogue */
                mono_raw = samples[src_idx * channels + 2];
            } else if (channels == 2) {
                /* Stereo source: downmix L and R */
                mono_raw = ((int32_t)samples[src_idx * 2 + 0] + (int32_t)samples[src_idx * 2 + 1]) / 2;
            } else {
                mono_raw = samples[src_idx * channels + 0];
            }

            /* Telephone DSP: bandpass filtering (350 Hz - 3400 Hz) */
            float s = (float)mono_raw;
            s = biquad_process(&s_hp_filter, s);
            s = biquad_process(&s_lp_filter, s);

            /* Clean limiting without digital clipping distortion */
            if (s > 32767.0f) s = 32767.0f;
            else if (s < -32768.0f) s = -32768.0f;

            int16_t val = (int16_t)s;
            if (s_is_stereo_port) {
                grain_buf[i * 2 + 0] = val;
                grain_buf[i * 2 + 1] = val;
            } else {
                grain_buf[i] = val;
            }
        }

        /* Zero pad tail if last chunk is shorter than s_pad_grain */
        if (chunk_frames < (size_t)s_pad_grain) {
            size_t tail_samples = s_pad_grain - chunk_frames;
            if (s_is_stereo_port) {
                memset(&grain_buf[chunk_frames * 2], 0, tail_samples * 2 * sizeof(int16_t));
            } else {
                memset(&grain_buf[chunk_frames], 0, tail_samples * sizeof(int16_t));
            }
        }

        /* Push grain to ring buffer */
        pthread_mutex_lock(&s_mutex);
        if (s_ring_count < PAD_RING_BLOCKS) {
            size_t grain_bytes = (size_t)s_pad_grain * (s_is_stereo_port ? 2 : 1) * sizeof(int16_t);
            memcpy(s_ring[s_ring_head], grain_buf, grain_bytes);
            s_ring_head = (s_ring_head + 1) % PAD_RING_BLOCKS;
            s_ring_count++;
            pthread_cond_signal(&s_cond);
        }
        pthread_mutex_unlock(&s_mutex);
    }
}
