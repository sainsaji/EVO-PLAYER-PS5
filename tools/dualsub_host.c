/*
 * tools/dualsub_host.c - host test for the dual-subtitle engine (#110).
 * Built and run by tools/dualsub_host.sh against a host FFmpeg.
 *
 * Links media/src/evo_subtitle.c - the file the app module compiles - and
 * drives it the way the app does: an MKV with three embedded SubRip tracks and
 * a sidecar .srt is opened, the demux thread's job (hand each packet to the
 * engine) is done by feed_all(), and the picker's job (select a track) by
 * calling the same functions it calls.
 *
 * What it checks:
 *   - a secondary track decodes into its own ring and reads independently of
 *     the primary, with its own delay
 *   - the same source cannot be both tracks, in either direction
 *   - what was chosen survives a reopen of the same file, and is dropped for
 *     a different one
 *   - a seek empties both rings
 *   - the external SRT works as either track, never both
 *   - one demux thread feeding packets while the UI thread switches, clears
 *     and reads the tracks. Build with SAN=thread to have the race detector
 *     check the two-lock design; the default (address,undefined) still
 *     catches a decoder freed under a decode in flight.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavformat/avformat.h>

#include "evo_subtitle.h"

/* ---- what the engine expects the app to own ------------------------------ */
int                screen = 2;
int                player_paused;
double             video_clock_seconds;
AVFormatContext   *play_fmt;
AVCodecContext    *audio_ctx;
char               current_media_path[768];
double             resume_base_offset_seconds;
double             requested_resume_seek_pos;
long long          controls_last_used_ms;
int                audio_stream_index = -1;
int                audio_handle;
volatile double    audio_clock_seconds;

static int g_verbose;

void toast(const char *title, const char *msg)
{
    if (g_verbose) fprintf(stderr, "    toast: %s | %s\n", title, msg);
}
long long now_ms(void) { return 0; }
int start_video_playback_at(const char *path, double resume_seconds)
{
    (void)path; (void)resume_seconds;
    return 1;
}
void evo_boot_log(const char *fmt, ...)
{
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("    | ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- checks -------------------------------------------------------------- */
static int g_fail, g_pass;

#define CHECK(cond, ...)                                            \
    do {                                                            \
        if (cond) { g_pass++; }                                     \
        else {                                                      \
            g_fail++;                                               \
            fprintf(stderr, "  FAIL %s:%d  ", __FILE__, __LINE__);  \
            fprintf(stderr, __VA_ARGS__);                           \
            fputc('\n', stderr);                                    \
        }                                                           \
    } while (0)

/* ---- the test film -------------------------------------------------------- */
typedef struct { double s, e; const char *text; } cue_t;

static const cue_t k_eng[] = { {1, 3, "EN one"},  {5, 7, "EN two"},  {9, 11, "EN three"} };
static const cue_t k_spa[] = { {1.5, 3.5, "ES uno"}, {5.5, 7.5, "ES dos"}, {9.5, 11.5, "ES tres"} };
static const cue_t k_fra[] = { {2, 4, "FR un"},   {6, 8, "FR deux"}, {10, 12, "FR trois"} };
static const char *k_lang[3] = { "eng", "spa", "fra" };
static const cue_t *k_tracks[3] = { k_eng, k_spa, k_fra };

typedef struct { int track; const cue_t *cue; } item_t;

static int item_cmp(const void *a, const void *b)
{
    double d = ((const item_t *)a)->cue->s - ((const item_t *)b)->cue->s;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static int write_mkv(const char *path)
{
    AVFormatContext *oc = NULL;
    int rc = -1;

    if (avformat_alloc_output_context2(&oc, NULL, "matroska", path) < 0) return -1;
    for (int i = 0; i < 3; i++) {
        AVStream *st = avformat_new_stream(oc, NULL);
        st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        st->codecpar->codec_id = AV_CODEC_ID_SUBRIP;
        st->time_base = (AVRational){ 1, 1000 };
        av_dict_set(&st->metadata, "language", k_lang[i], 0);
    }
    if (avio_open(&oc->pb, path, AVIO_FLAG_WRITE) < 0) goto done;
    if (avformat_write_header(oc, NULL) < 0) goto done;

    item_t items[9];
    int n = 0;
    for (int t = 0; t < 3; t++)
        for (int c = 0; c < 3; c++) { items[n].track = t; items[n].cue = &k_tracks[t][c]; n++; }
    qsort(items, (size_t)n, sizeof(items[0]), item_cmp);

    for (int i = 0; i < n; i++) {
        const cue_t *c = items[i].cue;
        AVPacket *pkt = av_packet_alloc();
        av_new_packet(pkt, (int)strlen(c->text));
        memcpy(pkt->data, c->text, strlen(c->text));
        pkt->stream_index = items[i].track;
        pkt->pts = pkt->dts = (int64_t)(c->s * 1000);
        pkt->duration = (int64_t)((c->e - c->s) * 1000);
        av_interleaved_write_frame(oc, pkt);
        av_packet_free(&pkt);
    }
    av_write_trailer(oc);
    rc = 0;
done:
    if (oc && oc->pb) avio_closep(&oc->pb);
    avformat_free_context(oc);
    return rc;
}

static void write_srt(const char *path)
{
    FILE *f = fopen(path, "w");
    fputs("1\n00:00:01,000 --> 00:00:03,000\nSRT one\n\n"
          "2\n00:00:05,000 --> 00:00:07,000\nSRT two\n\n"
          "3\n00:00:09,000 --> 00:00:11,000\nSRT three\n\n", f);
    fclose(f);
}

/* What the demux thread does: hand the engine every packet a slot follows. */
static void feed(AVFormatContext *fmt)
{
    AVPacket *pkt = av_packet_alloc();
    av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
    while (av_read_frame(fmt, pkt) >= 0) {
        if (prospero_subtitle_wants_stream(pkt->stream_index))
            prospero_embedded_subtitle_decode_packet(pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

static void feed_all(void) { feed(play_fmt); }

static const char *primary_at(double t)
{
    static char buf[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE];
    prospero_embedded_subtitle_text_at(t, buf, sizeof(buf));
    return buf;
}

static const char *secondary_at(double clock)
{
    static char buf[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE];
    prospero_secondary_subtitle_text_at(clock, buf, sizeof(buf));
    return buf;
}

/* One reopen of the same file, as PlaybackController does for an audio change. */
static void reopen(void)
{
    prospero_embedded_subtitle_open(play_fmt);
    prospero_secondary_subtitle_open(play_fmt);
}

/* ---- stress: demux thread vs the UI thread -------------------------------- */
static volatile int g_stop;
static const char  *g_path;

static void *feeder(void *arg)
{
    (void)arg;
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, g_path, NULL, NULL) < 0) return NULL;
    avformat_find_stream_info(fmt, NULL);
    while (!g_stop) feed(fmt);
    avformat_close_input(&fmt);
    return NULL;
}

static void stress(void)
{
    pthread_t th;
    char sink[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE];

    g_stop = 0;
    pthread_create(&th, NULL, feeder, NULL);

    for (int i = 0; i < 4000; i++) {
        switch (i % 7) {
        case 0: prospero_secondary_subtitle_select(1); break;
        case 1: prospero_secondary_subtitle_select(2); break;
        case 2: prospero_secondary_subtitle_select(PROSPERO_SECONDARY_NONE); break;
        case 3: prospero_subtitle_select_primary(1); break;
        case 4: prospero_subtitle_select_primary(0); break;
        case 5: prospero_embedded_subtitle_reset(); break;
        case 6: prospero_secondary_subtitle_select(0); break;
        }
        prospero_secondary_subtitle_text_at(2.0 + (i % 9), sink, sizeof(sink));
        prospero_embedded_subtitle_text_at(2.0 + (i % 9), sink, sizeof(sink));
    }

    g_stop = 1;
    pthread_join(th, NULL);
    CHECK(1, "stress finished");
}

/* ---- the run --------------------------------------------------------------- */
int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    g_verbose = argc > 2 && !strcmp(argv[2], "-v");

    char mkv[512], srt[512];
    snprintf(mkv, sizeof mkv, "%s/dualsub.mkv", dir);
    snprintf(srt, sizeof srt, "%s/dualsub.srt", dir);
    g_path = mkv;

    if (write_mkv(mkv) < 0) { fprintf(stderr, "could not write %s\n", mkv); return 2; }
    write_srt(srt);

    if (avformat_open_input(&play_fmt, mkv, NULL, NULL) < 0 ||
        avformat_find_stream_info(play_fmt, NULL) < 0) {
        fprintf(stderr, "could not open %s\n", mkv);
        return 2;
    }
    snprintf(current_media_path, sizeof current_media_path, "%s", mkv);
    CHECK(play_fmt->nb_streams == 3, "expected 3 subtitle streams, got %u", play_fmt->nb_streams);

    fprintf(stderr, "== primary and secondary decode independently\n");
    prospero_subtitle_requested_stream = 0;
    CHECK(prospero_embedded_subtitle_open(play_fmt) == 1, "primary open");
    CHECK(prospero_embedded_subtitle_stream_index == 0, "primary is stream %d", prospero_embedded_subtitle_stream_index);
    CHECK(prospero_secondary_subtitle_open(play_fmt) == 0, "no secondary chosen yet");
    CHECK(!prospero_secondary_subtitle_active(), "secondary inactive at start");

    CHECK(prospero_secondary_subtitle_select(1) == 1, "select spa as secondary");
    CHECK(prospero_secondary_subtitle_active(), "secondary active");
    CHECK(prospero_secondary_subtitle_stream_index == 1, "secondary stream is %d", prospero_secondary_subtitle_stream_index);
    CHECK(prospero_subtitle_wants_stream(0) && prospero_subtitle_wants_stream(1) && !prospero_subtitle_wants_stream(2),
          "the demuxer wants exactly streams 0 and 1");

    feed_all();
    CHECK(!strcmp(primary_at(2.0),   "EN one"), "primary at 2.0 = '%s'", primary_at(2.0));
    CHECK(!strcmp(secondary_at(2.0), "ES uno"), "secondary at 2.0 = '%s'", secondary_at(2.0));
    CHECK(!strcmp(primary_at(6.0),   "EN two"), "primary at 6.0 = '%s'", primary_at(6.0));
    CHECK(!strcmp(secondary_at(6.0), "ES dos"), "secondary at 6.0 = '%s'", secondary_at(6.0));
    CHECK(!strcmp(secondary_at(4.5), ""),       "secondary at 4.5 (between cues) = '%s'", secondary_at(4.5));
    CHECK(!strcmp(primary_at(3.2),   ""),       "primary at 3.2 (between cues) = '%s'", primary_at(3.2));

    fprintf(stderr, "== the secondary has its own delay\n");
    for (int i = 0; i < 10; i++) prospero_secondary_nudge_delay(+100);
    CHECK(prospero_secondary_delay_ms == 1000, "delay is %d", prospero_secondary_delay_ms);
    CHECK(!strcmp(secondary_at(2.5), "ES uno"), "delayed secondary at 2.5 = '%s'", secondary_at(2.5));
    CHECK(!strcmp(secondary_at(1.0), ""),       "delayed secondary at 1.0 = '%s'", secondary_at(1.0));
    CHECK(prospero_subtitle_delay_ms == 0, "the primary delay is untouched");
    CHECK(!strcmp(primary_at(2.0), "EN one"), "the primary is untouched by the secondary's delay");
    for (int i = 0; i < 10; i++) prospero_secondary_nudge_delay(-100);
    CHECK(prospero_secondary_delay_ms == 0, "delay back to %d", prospero_secondary_delay_ms);

    fprintf(stderr, "== one source is never both tracks\n");
    CHECK(prospero_secondary_subtitle_select(0) == 0, "the primary cannot also be the secondary");
    CHECK(prospero_secondary_subtitle_stream_index == 1, "a refused select leaves the secondary alone (%d)",
          prospero_secondary_subtitle_stream_index);
    CHECK(prospero_secondary_subtitle_select(7) == 0, "a stream that does not exist is refused");
    CHECK(prospero_secondary_subtitle_select(1) == 1, "reselecting the current secondary is fine");

    fprintf(stderr, "== switching the secondary empties its ring, then refills\n");
    CHECK(prospero_secondary_subtitle_select(2) == 1, "switch to fra");
    CHECK(!strcmp(secondary_at(2.0), ""), "the old track's cue is gone at once ('%s')", secondary_at(2.0));
    feed_all();
    CHECK(!strcmp(secondary_at(2.0), "FR un"), "fra at 2.0 = '%s'", secondary_at(2.0));
    CHECK(!strcmp(primary_at(2.0), "EN one"), "the primary kept its cues through the switch");

    fprintf(stderr, "== clearing the secondary\n");
    CHECK(prospero_secondary_subtitle_select(PROSPERO_SECONDARY_NONE) == 1, "clear");
    CHECK(!prospero_secondary_subtitle_active(), "inactive after clear");
    CHECK(!strcmp(secondary_at(2.0), ""), "no text after clear");
    CHECK(!prospero_subtitle_wants_stream(2), "the demuxer no longer wants stream 2");
    CHECK(prospero_secondary_subtitle_stream_index == -1, "slot closed (%d)", prospero_secondary_subtitle_stream_index);

    fprintf(stderr, "== the choice is per file\n");
    CHECK(prospero_secondary_subtitle_select(1) == 1, "select spa");
    reopen();
    CHECK(prospero_secondary_subtitle_active() && prospero_secondary_subtitle_stream_index == 1,
          "same file reopened: secondary kept (stream %d)", prospero_secondary_subtitle_stream_index);
    char other[] = "/mnt/usb0/some/other.mkv";
    char keep[768];
    snprintf(keep, sizeof keep, "%s", current_media_path);
    snprintf(current_media_path, sizeof current_media_path, "%s", other);
    reopen();
    CHECK(!prospero_secondary_subtitle_active(), "a different file starts without a secondary");
    CHECK(prospero_secondary_subtitle_requested == PROSPERO_SECONDARY_NONE, "and forgets the request");
    snprintf(current_media_path, sizeof current_media_path, "%s", keep);
    reopen();
    CHECK(!prospero_secondary_subtitle_active(), "going back does not resurrect it");

    fprintf(stderr, "== a new primary takes the track from the secondary\n");
    CHECK(prospero_secondary_subtitle_select(1) == 1, "secondary = spa");
    CHECK(prospero_subtitle_select_primary(1) == 1, "primary = spa");
    CHECK(prospero_embedded_subtitle_stream_index == 1, "primary stream is %d", prospero_embedded_subtitle_stream_index);
    CHECK(!prospero_secondary_subtitle_active(), "the secondary was dropped");
    feed_all();
    CHECK(!strcmp(primary_at(2.0), "ES uno"), "primary decodes the new track: '%s'", primary_at(2.0));
    prospero_subtitle_select_primary(0);
    CHECK(prospero_embedded_subtitle_stream_index == 0, "and back to stream 0");

    fprintf(stderr, "== a seek empties both rings\n");
    CHECK(prospero_secondary_subtitle_select(1) == 1, "secondary = spa");
    feed_all();
    CHECK(!strcmp(primary_at(2.0), "EN one") && !strcmp(secondary_at(2.0), "ES uno"), "both filled");
    prospero_embedded_subtitle_reset();
    CHECK(!strcmp(primary_at(2.0), "") && !strcmp(secondary_at(2.0), ""), "both empty after reset");
    CHECK(prospero_secondary_subtitle_active(), "the tracks themselves survive a seek");

    fprintf(stderr, "== the external SRT as either track\n");
    CHECK(prospero_subtitle_load_for_media(mkv) == 3, "sidecar loaded");
    prospero_subtitle_use_external = 0;
    CHECK(prospero_secondary_subtitle_select(-1) == 1, "SRT as secondary");
    CHECK(prospero_secondary_use_external && prospero_secondary_subtitle_active(), "external secondary active");
    CHECK(!strcmp(secondary_at(2.0), "SRT one"), "SRT text at 2.0 = '%s'", secondary_at(2.0));
    CHECK(prospero_secondary_subtitle_stream_index == -1, "no embedded stream held while the SRT is the secondary");
    CHECK(prospero_subtitle_select_primary(-1) == 1, "SRT as primary");
    CHECK(!prospero_secondary_subtitle_active(), "the secondary lost the SRT to the primary");
    CHECK(prospero_secondary_subtitle_select(-1) == 0, "the SRT cannot be both tracks");
    CHECK(prospero_secondary_subtitle_select(1) == 1, "an embedded track can still be the secondary");
    prospero_subtitle_select_primary(0);
    CHECK(!prospero_subtitle_use_external && prospero_embedded_subtitle_stream_index == 0, "primary back to embedded");

    fprintf(stderr, "== demux thread vs UI thread\n");
    prospero_secondary_subtitle_select(PROSPERO_SECONDARY_NONE);
    stress();

    prospero_embedded_subtitle_close();
    avformat_close_input(&play_fmt);
    remove(mkv);
    remove(srt);

    fprintf(stderr, "\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
