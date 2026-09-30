/*
 * tools/streamopts_host.c - host test for the network open options in
 * media/src/evo_stream_io.c.
 * Built and run by tools/streamopts_host.sh against the host FFmpeg.
 *
 * Links evo_stream_io.c - the file the app module compiles - and checks the
 * policy, not the network: which URLs count as a playlist, and which FFmpeg
 * options a network open is given.
 *
 * The case that motivated it (hardware, 2026-09-30): reconnect_at_eof was set for
 * every network URL. For an HLS playlist that turns the end of a small file into
 * a reconnect - a full request and TLS handshake - repeated with a growing delay
 * on every playlist the demuxer reads, and libavformat's hls demuxer hands the
 * option to all of them. Opening a channel took 10.8 s and 21.4 s.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/dict.h>

#include "evo_stream_io.h"

/* What evo_stream_io.c takes from the rest of the app. It never opens a stream
 * in this test, so neither is called. */
void *evo_direct_mem_calloc(size_t n, size_t size) { return calloc(n, size); }
void  evo_direct_mem_free(void *p) { free(p); }

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

static const char *opt(AVDictionary *d, const char *key)
{
    AVDictionaryEntry *e = av_dict_get(d, key, NULL, 0);
    return e ? e->value : NULL;
}

static int opt_is(AVDictionary *d, const char *key, const char *want)
{
    const char *v = opt(d, key);
    return v && !strcmp(v, want);
}

int main(void)
{
    fprintf(stderr, "== which URLs are playlists\n");
    static const char *playlists[] = {
        "https://raw.githubusercontent.com/x/y/AsianetHD.m3u8",
        "https://cdn.example/live/index.M3U8",
        "https://cdn.example/live/playlist.m3u8?token=abc&exp=1",
        "https://cdn.example/28072023/smil:asianetmovies1.smil/playlist.m3u8",
        "https://cdn.example/dash/manifest.mpd",
        "https://cdn.example/dash/MANIFEST.MPD?x=1",
        "https://cdn.example/live.isml/live.ism/manifest",
    };
    static const char *raw[] = {
        "http://up.kiwi/351921603109/34939156/1293823",     /* an extensionless live channel */
        "http://panel.example:8080/live/user/pass/1234.ts",
        "https://cdn.example/movie.mp4",
        "/mnt/usb0/media/Movies/film.mkv",
        "http://host/m3u8",                                 /* no dot: not an extension */
        "",
    };
    for (size_t i = 0; i < sizeof playlists / sizeof playlists[0]; i++)
        CHECK(evo_stream_io_url_is_playlist(playlists[i]), "a playlist: %s", playlists[i]);
    for (size_t i = 0; i < sizeof raw / sizeof raw[0]; i++)
        CHECK(!evo_stream_io_url_is_playlist(raw[i]), "not a playlist: '%s'", raw[i]);
    CHECK(!evo_stream_io_url_is_playlist(NULL), "NULL is not a playlist");

    fprintf(stderr, "== options for an HLS playlist\n");
    AVDictionary *hls = NULL;
    evo_stream_io_apply_network_options(&hls, "https://cdn.example/live/index.m3u8");
    CHECK(opt(hls, "reconnect_at_eof") == NULL,
          "reconnect_at_eof is NOT set: it turns every small playlist into a reconnect loop (got '%s')",
          opt(hls, "reconnect_at_eof") ? opt(hls, "reconnect_at_eof") : "");
    CHECK(opt_is(hls, "reconnect", "1"), "reconnect stays on for a dropped connection");
    CHECK(opt_is(hls, "reconnect_on_network_error", "1"), "and on a network error");
    CHECK(opt_is(hls, "reconnect_max_retries", "3"), "retries are capped at 3");
    CHECK(opt_is(hls, "reconnect_delay_total_max", "6"), "total waiting is capped at 6 s");
    CHECK(opt_is(hls, "reconnect_delay_max", "2"), "one delay is capped at 2 s");
    CHECK(opt_is(hls, "timeout", "5000000") && opt_is(hls, "rw_timeout", "5000000"), "5 s timeouts");
    CHECK(opt_is(hls, "allowed_extensions", "ALL"), "segment URLs of any shape are accepted");
    CHECK(opt_is(hls, "extension_picky", "0"),
          "and an extensionless segment is not refused by the second gate (hls.c test_segment)");
    av_dict_free(&hls);

    fprintf(stderr, "== options for a raw live stream\n");
    AVDictionary *ts = NULL;
    evo_stream_io_apply_network_options(&ts, "http://up.kiwi/351921603109/34939156/1293823");
    CHECK(opt_is(ts, "reconnect_at_eof", "1"), "reconnect_at_eof stays on: an EOF here is a dropped connection");
    CHECK(opt_is(ts, "reconnect", "1") && opt_is(ts, "reconnect_streamed", "1"), "reconnect and reconnect_streamed");
    CHECK(opt_is(ts, "reconnect_max_retries", "3") && opt_is(ts, "reconnect_delay_total_max", "6"),
          "the same bounds");
    av_dict_free(&ts);

    fprintf(stderr, "== a DASH manifest\n");
    AVDictionary *dash = NULL;
    evo_stream_io_apply_network_options(&dash, "https://cdn.example/dash/manifest.mpd?x=1");
    CHECK(opt(dash, "reconnect_at_eof") == NULL, "no reconnect_at_eof for DASH either");
    av_dict_free(&dash);

    fprintf(stderr, "\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
