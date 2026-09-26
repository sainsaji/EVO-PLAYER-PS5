/*
 * tools/subsync_host.c - host harness for media/src/evo_subsync.c (#102).
 * Built and run by tools/subsync_host.sh against a host FFmpeg.
 *
 *   subsync_host synth <dir>
 *       Self-test. Writes <dir>/synth.wav: a 30 min "film" whose speech
 *       follows a random cue list (with jittered edges, cues without speech
 *       and speech without cues, a noise bed and loud non-speech stretches),
 *       then runs the analysis against that cue list shifted, rescaled, both,
 *       and against a cue list from another "film". Exit 0 = all recovered.
 *
 *       The same film is then muxed into MKVs with the cues as an embedded
 *       SubRip track (in sync, shifted, another film's) for the embedded path.
 *
 *   subsync_host <media> <srt> [shift_s [scale]]
 *       Real clip. Loads <srt>, optionally corrupts it (t' = t*scale + shift),
 *       analyses, and prints the offset/ratio that would be applied.
 *
 *   subsync_host <media> --stream <n>
 *       Real clip, embedded text track n. Prints the offset it would apply.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>

#include "evo_subsync.h"

/* The app's log goes to evo.log; here it goes to stderr. */
void evo_boot_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("  | ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#define PI 3.14159265358979323846

/* ---- tiny deterministic RNG ---- */
static uint64_t g_rng = 1;
static double rnd(void)
{
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(g_rng >> 11) / (double)(1ULL << 53);
}
static double rnd_range(double a, double b) { return a + (b - a) * rnd(); }

typedef struct { double *s, *e; int n; } cues_t;

static void cues_free(cues_t *c) { free(c->s); free(c->e); memset(c, 0, sizeof(*c)); }

static cues_t cues_random(uint64_t seed, double film_s)
{
    cues_t c = {0};
    int cap = 4096;
    c.s = malloc(sizeof(double) * cap);
    c.e = malloc(sizeof(double) * cap);
    g_rng = seed;
    double t = rnd_range(20.0, 60.0);
    while (t < film_s - 10.0 && c.n < cap) {
        double len = rnd_range(0.9, 4.5);
        c.s[c.n] = t;
        c.e[c.n] = t + len;
        c.n++;
        /* mostly conversational gaps, sometimes a long action stretch */
        t += len + (rnd() < 0.12 ? rnd_range(15.0, 70.0) : rnd_range(0.3, 5.0));
    }
    return c;
}

static cues_t cues_transform(const cues_t *in, double scale, double shift)
{
    cues_t c = {0};
    c.n = in->n;
    c.s = malloc(sizeof(double) * c.n);
    c.e = malloc(sizeof(double) * c.n);
    for (int i = 0; i < c.n; i++) {
        c.s[i] = in->s[i] * scale + shift;
        c.e[i] = in->e[i] * scale + shift;
    }
    return c;
}

/* ---- synthetic film ---- */
static int write_wav(const char *path, const int16_t *pcm, int64_t n, int rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t data = (uint32_t)(n * 2), riff = 36 + data, fmtlen = 16, sr = rate,
             br = rate * 2;
    uint16_t pcmfmt = 1, ch = 1, ba = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtlen, 4, 1, f); fwrite(&pcmfmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 2, (size_t)n, f);
    fclose(f);
    return 0;
}

static void add_speech(float *buf, int64_t total, int rate, double a, double b)
{
    /* syllables: 3 formant-ish carriers under a ~4.5 Hz raised-cosine envelope */
    double f1 = rnd_range(450, 800), f2 = rnd_range(1100, 1800), f3 = rnd_range(2200, 3000);
    double syl = rnd_range(3.5, 5.5), amp = rnd_range(0.10, 0.30);
    int64_t i0 = (int64_t)(a * rate), i1 = (int64_t)(b * rate);
    if (i0 < 0) i0 = 0;
    if (i1 > total) i1 = total;
    for (int64_t i = i0; i < i1; i++) {
        double t = (double)(i - i0) / rate;
        double env = 0.5 - 0.5 * cos(2 * PI * syl * t);
        double v = sin(2 * PI * f1 * t) + 0.6 * sin(2 * PI * f2 * t) + 0.3 * sin(2 * PI * f3 * t);
        buf[i] += (float)(amp * env * v / 1.9);
    }
}

static int synth_film(const char *path, const cues_t *truth, double film_s, int rate)
{
    int64_t n = (int64_t)(film_s * rate);
    float *buf = calloc((size_t)n, sizeof(float));
    int16_t *pcm = malloc(sizeof(int16_t) * (size_t)n);
    if (!buf || !pcm) return -1;

    g_rng = 777;
    /* noise bed (-45 dBFS-ish) */
    for (int64_t i = 0; i < n; i++) buf[i] = (float)((rnd() - 0.5) * 0.012);
    /* loud non-speech: rumble stretches (below the band) and a few music cues */
    for (double t = 30.0; t < film_s; t += rnd_range(60.0, 240.0)) {
        double len = rnd_range(5.0, 40.0), f = rnd_range(40.0, 120.0);
        for (int64_t i = (int64_t)(t * rate); i < (int64_t)((t + len) * rate) && i < n; i++)
            buf[i] += (float)(0.35 * sin(2 * PI * f * (double)i / rate));
    }
    for (double t = 90.0; t < film_s; t += rnd_range(300.0, 600.0)) {
        double len = rnd_range(20.0, 60.0);
        for (int64_t i = (int64_t)(t * rate); i < (int64_t)((t + len) * rate) && i < n; i++) {
            double x = (double)i / rate;
            buf[i] += (float)(0.06 * (sin(2 * PI * 440 * x) + sin(2 * PI * 554 * x)) *
                              (0.6 + 0.4 * sin(2 * PI * 0.5 * x)));
        }
    }
    /* speech on 85% of cues, edges off by up to 0.25 s */
    for (int i = 0; i < truth->n; i++) {
        if (rnd() < 0.15) continue;
        add_speech(buf, n, rate, truth->s[i] + rnd_range(-0.25, 0.25),
                   truth->e[i] + rnd_range(-0.25, 0.25));
    }
    /* un-subtitled chatter */
    for (double t = 50.0; t < film_s; t += rnd_range(40.0, 120.0))
        add_speech(buf, n, rate, t, t + rnd_range(0.5, 2.0));

    for (int64_t i = 0; i < n; i++) {
        float v = buf[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = (int16_t)(v * 32767.0f);
    }
    int rc = write_wav(path, pcm, n, rate);
    free(buf);
    free(pcm);
    return rc;
}

/* ---- SRT ---- */
static cues_t srt_load(const char *path)
{
    cues_t c = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return c;
    int cap = 8192;
    c.s = malloc(sizeof(double) * cap);
    c.e = malloc(sizeof(double) * cap);
    char line[1024];
    while (fgets(line, sizeof(line), f) && c.n < cap) {
        int h1, m1, s1, ms1, h2, m2, s2, ms2;
        if (sscanf(line, "%d:%d:%d%*[,.]%d --> %d:%d:%d%*[,.]%d",
                   &h1, &m1, &s1, &ms1, &h2, &m2, &s2, &ms2) == 8) {
            c.s[c.n] = h1 * 3600 + m1 * 60 + s1 + ms1 / 1000.0;
            c.e[c.n] = h2 * 3600 + m2 * 60 + s2 + ms2 / 1000.0;
            c.n++;
        }
    }
    fclose(f);
    return c;
}

/* ---- the synthetic film with the cues as an embedded SubRip track ---- */
static int mux_mkv(const char *wav, const cues_t *c, const char *out)
{
    AVFormatContext *in = NULL, *oc = NULL;
    AVPacket *pkt = av_packet_alloc();
    int rc = -1, ci = 0;
    if (avformat_open_input(&in, wav, NULL, NULL) < 0) goto done;
    if (avformat_find_stream_info(in, NULL) < 0) goto done;
    if (avformat_alloc_output_context2(&oc, NULL, "matroska", out) < 0) goto done;

    AVStream *as = avformat_new_stream(oc, NULL);
    avcodec_parameters_copy(as->codecpar, in->streams[0]->codecpar);
    as->codecpar->codec_tag = 0;
    as->time_base = in->streams[0]->time_base;
    AVStream *ss = avformat_new_stream(oc, NULL);
    ss->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    ss->codecpar->codec_id = AV_CODEC_ID_SUBRIP;
    ss->time_base = (AVRational){ 1, 1000 };
    if (avio_open(&oc->pb, out, AVIO_FLAG_WRITE) < 0) goto done;
    if (avformat_write_header(oc, NULL) < 0) goto done;

    const AVRational ms = { 1, 1000 };
    while (av_read_frame(in, pkt) >= 0) {
        double t = pkt->pts * av_q2d(in->streams[0]->time_base);
        for (; ci < c->n && c->s[ci] <= t; ci++) {
            if (c->s[ci] < 0.0) continue;
            AVPacket *sp = av_packet_alloc();
            av_new_packet(sp, 5);
            memcpy(sp->data, "Line.", 5);
            sp->stream_index = 1;
            sp->pts = sp->dts = av_rescale_q((int64_t)(c->s[ci] * 1000), ms, ss->time_base);
            sp->duration = av_rescale_q((int64_t)((c->e[ci] - c->s[ci]) * 1000), ms, ss->time_base);
            av_interleaved_write_frame(oc, sp);
            av_packet_free(&sp);
        }
        av_packet_rescale_ts(pkt, in->streams[0]->time_base, as->time_base);
        pkt->stream_index = 0;
        av_interleaved_write_frame(oc, pkt);
    }
    av_write_trailer(oc);
    rc = 0;
done:
    av_packet_free(&pkt);
    if (oc && oc->pb) avio_closep(&oc->pb);
    avformat_free_context(oc);
    avformat_close_input(&in);
    return rc;
}

/* ---- runner ---- */
static int run_case(const char *name, const char *media, int sub_stream,
                    const cues_t *c, int expect_ok, double expect_delay,
                    double expect_scale)
{
    evo_subsync_result_t r;
    volatile int cancel = 0, progress = 0;
    fprintf(stderr, "\n== %s\n", name);
    evo_subsync_analyse(media, -1, sub_stream, c ? c->s : NULL, c ? c->e : NULL,
                        c ? c->n : 0, &cancel, &progress, &r);
    const char *ratio = evo_subsync_ratio_label(r.scale);
    printf("%-28s status=%d delay=%+.3fs scale=%.5f%s%s conf=%.3f windows=%d %.1fs",
           name, r.status, r.delay_s, r.scale, ratio ? " " : "", ratio ? ratio : "",
           r.confidence, r.windows_used, r.elapsed_s);
    int pass;
    if (!expect_ok)
        pass = r.status == EVO_SUBSYNC_LOW_CONF;
    else
        pass = r.status == EVO_SUBSYNC_OK && fabs(r.delay_s - expect_delay) <= 0.100 &&
               fabs(r.scale - expect_scale) < 1e-4;
    printf("  -> %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "synth")) {
        const double film = 30 * 60.0;
        char wav[1024];
        snprintf(wav, sizeof(wav), "%s/synth.wav", argv[2]);
        cues_t truth = cues_random(42, film);
        fprintf(stderr, "writing %s (%d cues)\n", wav, truth.n);
        if (synth_film(wav, &truth, film, 16000) < 0) {
            fprintf(stderr, "synth failed\n");
            return 2;
        }

        /* An SRT whose cues say t' = t*k + b is fixed by delay = -b, scale = k:
         * the player shows cue time clock*scale - delay. */
        const double pal = (24000.0 / 1001.0) / 25.0;   /* 25 fps SRT on 23.976 */
        const double ntsc = (24000.0 / 1001.0) / 24.0;  /* 24 -> 23.976         */
        int pass = 0, total = 0;
        struct { const char *name; double k, b; } cases[] = {
            { "in sync",               1.0,   0.0 },
            { "shifted +3.7 s",        1.0,   3.7 },
            { "shifted -42.25 s",      1.0, -42.25 },
            { "25->23.976 fps",        pal,   0.0 },
            { "25->23.976 fps +1.8 s", pal,   1.8 },
            { "24->23.976 fps -0.6 s", ntsc, -0.6 },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            cues_t c = cues_transform(&truth, cases[i].k, cases[i].b);
            pass += run_case(cases[i].name, wav, -1, &c, 1, -cases[i].b, cases[i].k);
            total++;
            cues_free(&c);
        }
        cues_t other = cues_random(4242, film);
        pass += run_case("other film's SRT", wav, -1, &other, 0, 0, 0);
        total++;

        /* Embedded track: offset only. Cues muxed at t + b are fixed by
         * delay = -b, exactly like the SRT. */
        struct { const char *name, *file; const cues_t *src; double b; int ok; } emb[] = {
            { "embedded in sync",       "synth_emb_0.mkv",  &truth, 0.0,  1 },
            { "embedded shifted +3.7 s","synth_emb_1.mkv",  &truth, 3.7,  1 },
            { "embedded shifted -12.3 s","synth_emb_2.mkv", &truth, -12.3, 1 },
            { "embedded other film",    "synth_emb_3.mkv",  &other, 0.0,  0 },
        };
        for (size_t i = 0; i < sizeof(emb) / sizeof(emb[0]); i++) {
            char mkv[1024];
            snprintf(mkv, sizeof(mkv), "%s/%s", argv[2], emb[i].file);
            cues_t c = cues_transform(emb[i].src, 1.0, emb[i].b);
            if (mux_mkv(wav, &c, mkv) < 0) {
                fprintf(stderr, "mux %s failed\n", mkv);
                return 2;
            }
            pass += run_case(emb[i].name, mkv, 1, NULL, emb[i].ok, -emb[i].b, 1.0);
            total++;
            cues_free(&c);
        }
        cues_free(&other);
        cues_free(&truth);
        printf("\n%d/%d passed\n", pass, total);
        return pass == total ? 0 : 1;
    }

    if (argc < 3) {
        fprintf(stderr, "usage: %s synth <dir> | <media> <srt> [shift_s [scale]]"
                        " | <media> --stream <n>\n", argv[0]);
        return 2;
    }
    if (argc >= 4 && !strcmp(argv[2], "--stream"))
        return run_case("clip (embedded)", argv[1], atoi(argv[3]), NULL, 1, 0.0, 1.0) ? 0 : 1;
    cues_t srt = srt_load(argv[2]);
    if (srt.n <= 0) {
        fprintf(stderr, "no cues in %s\n", argv[2]);
        return 2;
    }
    double shift = argc > 3 ? atof(argv[3]) : 0.0;
    double scale = argc > 4 ? atof(argv[4]) : 1.0;
    cues_t c = cues_transform(&srt, scale, shift);
    int ok = run_case("clip", argv[1], -1, &c, 1, -shift, scale);
    cues_free(&c);
    cues_free(&srt);
    return ok ? 0 : 1;
}
