/*
 * tools/hls_host.c - host test for projects/evoplayer/addons/src/evo_hls_variants.c.
 * Built and run by tools/hls_host.sh.
 *
 * The parser and the URL join are pure, so this exercises them directly on
 * playlists shaped like the ones IPTV channels really serve. The fetch is
 * driven through a stub of evo_net_request_async.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evo_hls_variants.h"
#include "evo_net.h"

/* ---- evo_net, stubbed: the test decides what "the server" answers ---- */
static evo_net_cb  g_cb;
static void       *g_cb_ud;
static int         g_queue_rc;      /* what evo_net_request_async returns */
static char        g_last_url[512];
static int         g_request_count;

int evo_net_request_async(const char *method, const char *url, const char *post_data,
                          const char **headers, int header_count, evo_net_cb cb, void *ud)
{
    (void)post_data; (void)headers; (void)header_count;
    g_request_count++;
    snprintf(g_last_url, sizeof g_last_url, "%s %s", method, url);
    if (g_queue_rc != 0) return g_queue_rc;
    g_cb = cb;
    g_cb_ud = ud;
    return 0;
}

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

static void join_is(const char *base, const char *ref, const char *want)
{
    char out[512];
    int rc = evo_hls_join_url(base, ref, out, sizeof out);
    CHECK(rc == 0 && !strcmp(out, want), "join(%s, %s) = '%s' (rc %d), want '%s'", base, ref,
          rc == 0 ? out : "", rc, want);
}

static int parse(const char *text, evo_stream_choice_t *v, int max, const char *base)
{
    return evo_hls_parse_master(text, strlen(text), base, v, max);
}

/* ---- fetch callback capture ---- */
static int                 g_var_calls, g_var_count;
static evo_stream_choice_t g_var_first;

static void on_variants(int count, const evo_stream_choice_t *variants, void *ud)
{
    (void)ud;
    g_var_calls++;
    g_var_count = count;
    if (count > 0) g_var_first = variants[0];
}

int main(void)
{
    evo_stream_choice_t v[EVO_HLS_MAX_VARIANTS];
    const char *base = "https://cdn.example/live/ch1/index.m3u8?sig=abc";

    fprintf(stderr, "== URL join\n");
    join_is("https://cdn.example/live/index.m3u8", "720p.m3u8", "https://cdn.example/live/720p.m3u8");
    join_is("https://cdn.example/live/index.m3u8", "/other/1080.m3u8", "https://cdn.example/other/1080.m3u8");
    join_is("https://cdn.example/live/index.m3u8", "//img.example/x.m3u8", "https://img.example/x.m3u8");
    join_is("https://cdn.example/live/index.m3u8", "https://a.example/b.m3u8", "https://a.example/b.m3u8");
    join_is("https://cdn.example/live/v1/index.m3u8", "../hi/1080.m3u8", "https://cdn.example/live/hi/1080.m3u8");
    join_is("https://cdn.example/live/index.m3u8", "./sub/x.m3u8", "https://cdn.example/live/sub/x.m3u8");
    join_is("https://cdn.example/live/index.m3u8?sig=abc", "a.m3u8?token=1", "https://cdn.example/live/a.m3u8?token=1");
    join_is("https://cdn.example/live/index.m3u8?sig=abc", "?x=2", "https://cdn.example/live/index.m3u8?x=2");
    join_is("https://cdn.example", "a.m3u8", "https://cdn.example/a.m3u8");
    join_is("https://cdn.example/a/index.m3u8", "../../../x.m3u8", "https://cdn.example/x.m3u8");
    join_is("http://host:8080/a/b.m3u8", "/c.m3u8", "http://host:8080/c.m3u8");
    join_is("https://cdn.example/a/b/index.m3u8", "c/./d/../e.m3u8", "https://cdn.example/a/b/c/e.m3u8");
    join_is("https://cdn.example/a/index.m3u8#frag", "b.m3u8", "https://cdn.example/a/b.m3u8");
    {
        char out[512], tiny[8];
        CHECK(evo_hls_join_url("not a url", "x.m3u8", out, sizeof out) == -1, "a relative ref needs a URL base");
        CHECK(evo_hls_join_url(NULL, "x.m3u8", out, sizeof out) == -1, "NULL base");
        CHECK(evo_hls_join_url("https://cdn.example/a/b.m3u8", "c.m3u8", tiny, sizeof tiny) == -1,
              "a result that does not fit is refused");
        CHECK(evo_hls_join_url("not a url", "https://a.example/x", out, sizeof out) == 0 &&
              !strcmp(out, "https://a.example/x"), "an absolute ref needs no base");
    }

    fprintf(stderr, "== a master playlist\n");
    const char *master =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"en, stereo\",URI=\"audio.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1.42c01e,mp4a.40.2\"\n"
        "360/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2800000,AVERAGE-BANDWIDTH=2500000,RESOLUTION=1280x720,FRAME-RATE=25.000,CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
        "720/index.m3u8?t=1\n"
        "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=150000,RESOLUTION=1280x720,URI=\"iframes.m3u8\"\n"
        "\n"
        "# a comment between a tag and its URI would be legal too\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,CODECS=\"hvc1.1.6.L120.90,ec-3\"\n"
        "https://other.example/1080/index.m3u8\n";
    int n = parse(master, v, EVO_HLS_MAX_VARIANTS, base);
    CHECK(n == 3, "three variants (the I-frame playlist and the audio rendition are not variants): %d", n);
    CHECK(n == 3 && !strcmp(v[0].label, "1080p") && !strcmp(v[1].label, "720p") && !strcmp(v[2].label, "360p"),
          "best first: %s %s %s", v[0].label, v[1].label, v[2].label);
    CHECK(!strcmp(v[0].url, "https://other.example/1080/index.m3u8"), "an absolute URI is kept: %s", v[0].url);
    CHECK(!strcmp(v[1].url, "https://cdn.example/live/ch1/720/index.m3u8?t=1"),
          "a relative one is resolved against the master (its own query dropped): %s", v[1].url);
    CHECK(!strcmp(v[2].url, "https://cdn.example/live/ch1/360/index.m3u8"), "%s", v[2].url);
    CHECK(v[0].width == 1920 && v[0].height == 1080, "resolution %dx%d", v[0].width, v[0].height);
    CHECK(v[0].bitrate_bps == 5000000, "bitrate %lld", (long long)v[0].bitrate_bps);
    CHECK(!strcmp(v[0].video_codec, "hevc") && !strcmp(v[0].audio_codec, "eac3"),
          "codecs from a quoted list with a comma in it: %s / %s", v[0].video_codec, v[0].audio_codec);
    CHECK(!strcmp(v[1].video_codec, "h264") && !strcmp(v[1].audio_codec, "aac"), "%s / %s", v[1].video_codec, v[1].audio_codec);
    CHECK(v[1].bitrate_bps == 2800000, "BANDWIDTH wins over AVERAGE-BANDWIDTH: %lld", (long long)v[1].bitrate_bps);
    CHECK(!strcmp(v[0].container, "hls") && v[0].is_live == 1, "container and live flag");

    fprintf(stderr, "== the same playlist with CRLF line endings and blank lines\n");
    const char *crlf =
        "#EXTM3U\r\n\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360\r\n"
        "low.m3u8\r\n"
        "\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2800000,RESOLUTION=1280x720\r\n"
        "high.m3u8\r\n";
    n = parse(crlf, v, EVO_HLS_MAX_VARIANTS, "https://cdn.example/x/master.m3u8");
    CHECK(n == 2 && !strcmp(v[0].label, "720p") && !strcmp(v[0].url, "https://cdn.example/x/high.m3u8") &&
          !strcmp(v[1].url, "https://cdn.example/x/low.m3u8"), "CRLF: %d, %s", n, n ? v[0].url : "");

    fprintf(stderr, "== labels\n");
    const char *odd =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\n"
        "audio.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2400000\n"
        "mid.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000\n"
        "low.m3u8\n";
    n = parse(odd, v, EVO_HLS_MAX_VARIANTS, "https://cdn.example/m.m3u8");
    CHECK(n == 3, "three: %d", n);
    CHECK(n == 3 && !strcmp(v[0].label, "2.4 Mbps") && !strcmp(v[1].label, "800 kbps") && !strcmp(v[2].label, "Audio only"),
          "bitrate labels, audio-only last: '%s' '%s' '%s'", v[0].label, v[1].label, v[2].label);

    fprintf(stderr, "== not a master playlist\n");
    const char *media =
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:100\n"
        "#EXTINF:6.0,\nseg100.ts\n#EXTINF:6.0,\nseg101.ts\n";
    CHECK(parse(media, v, EVO_HLS_MAX_VARIANTS, base) == 0, "a media playlist has nothing to choose");
    CHECK(parse("", v, EVO_HLS_MAX_VARIANTS, base) == 0, "empty");
    CHECK(parse("<html>403 Forbidden</html>", v, EVO_HLS_MAX_VARIANTS, base) == 0, "an error page");
    CHECK(evo_hls_parse_master(NULL, 10, base, v, 6) == 0 && evo_hls_parse_master("x", 0, base, v, 6) == 0 &&
          evo_hls_parse_master("x", 1, base, NULL, 6) == 0 && evo_hls_parse_master("x", 1, base, v, 0) == 0,
          "bad arguments");
    CHECK(parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=1280x720\n", v, 6, base) == 0,
          "a STREAM-INF with no URI after it (a truncated download) is not a variant");
    CHECK(parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=1280x720\nhi.m3u8", v, 6, base) == 1,
          "a last URI with no newline is still read");

    fprintf(stderr, "== duplicates and the limit\n");
    const char *dup =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=1280x720\nsame.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=1280x720\nsame.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=1280x720\n./same.m3u8\n";
    CHECK(parse(dup, v, 6, "https://cdn.example/a/m.m3u8") == 1, "the same URL three ways is one variant");

    char many[4096] = "#EXTM3U\n";
    static const int heights[] = { 240, 1080, 360, 2160, 480, 720, 144, 576 };
    for (size_t i = 0; i < sizeof heights / sizeof heights[0]; i++) {
        char line[160];
        snprintf(line, sizeof line, "#EXT-X-STREAM-INF:BANDWIDTH=%d,RESOLUTION=%dx%d\nv%d.m3u8\n",
                 heights[i] * 1000, heights[i] * 16 / 9, heights[i], heights[i]);
        strcat(many, line);
    }
    n = parse(many, v, EVO_HLS_MAX_VARIANTS, "https://cdn.example/m.m3u8");
    CHECK(n == EVO_HLS_MAX_VARIANTS, "capped at %d (%d)", EVO_HLS_MAX_VARIANTS, n);
    CHECK(n == 6 && v[0].height == 2160 && v[1].height == 1080 && v[2].height == 720 && v[3].height == 576 &&
          v[4].height == 480 && v[5].height == 360,
          "the six best of eight, best first: %d %d %d %d %d %d", v[0].height, v[1].height, v[2].height,
          v[3].height, v[4].height, v[5].height);

    fprintf(stderr, "== the fetch\n");
    g_queue_rc = 0; g_var_calls = 0; g_request_count = 0;
    CHECK(evo_hls_variants_fetch("https://cdn.example/live/ch1/index.m3u8?sig=abc", on_variants, NULL) == 0, "queued");
    CHECK(g_request_count == 1 && !strcmp(g_last_url, "GET https://cdn.example/live/ch1/index.m3u8?sig=abc"),
          "a GET of the master: %s", g_last_url);
    CHECK(g_var_calls == 0, "nothing until the network answers");
    g_cb(1, 200, master, strlen(master), g_cb_ud);
    CHECK(g_var_calls == 1 && g_var_count == 3 && !strcmp(g_var_first.label, "1080p"),
          "the answer is parsed and delivered once: %d calls, %d variants", g_var_calls, g_var_count);

    g_var_calls = 0;
    CHECK(evo_hls_variants_fetch("https://cdn.example/x.m3u8", on_variants, NULL) == 0, "queued");
    g_cb(1, 403, "Forbidden", 9, g_cb_ud);
    CHECK(g_var_calls == 1 && g_var_count == 0, "HTTP 403: zero variants, still delivered exactly once");

    g_var_calls = 0;
    CHECK(evo_hls_variants_fetch("https://cdn.example/x.m3u8", on_variants, NULL) == 0, "queued");
    g_cb(0, 0, NULL, 0, g_cb_ud);
    CHECK(g_var_calls == 1 && g_var_count == 0, "a network failure: zero variants");

    g_var_calls = 0;
    CHECK(evo_hls_variants_fetch("https://cdn.example/x.m3u8", on_variants, NULL) == 0, "queued");
    g_cb(1, 200, media, strlen(media), g_cb_ud);
    CHECK(g_var_calls == 1 && g_var_count == 0, "a media playlist: zero variants");

    g_var_calls = 0;
    g_queue_rc = -1;
    CHECK(evo_hls_variants_fetch("https://cdn.example/x.m3u8", on_variants, NULL) < 0, "a request that cannot be queued is refused");
    CHECK(g_var_calls == 0, "and its callback never fires");
    CHECK(evo_hls_variants_fetch("", on_variants, NULL) < 0 && evo_hls_variants_fetch(NULL, on_variants, NULL) < 0 &&
          evo_hls_variants_fetch("https://x/y", NULL, NULL) < 0, "bad arguments");

    fprintf(stderr, "\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
