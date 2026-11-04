/*
 * evo_http_avio.c - one blocking HTTP request on FFmpeg's HTTP protocol.
 *
 * The Jellyfin client in Jelly5 (github.com/02dnot/Jelly5, GPL-3.0-or-later,
 * app/src/jf/jf_http_avio.cpp) sends its API calls through FFmpeg's own HTTP
 * client, the same network path the player streams through. This is that
 * request function in C, run by evo_net's worker thread for the requests that
 * ask for it (evo_net_request_async_avio). FFmpeg reports 4xx/5xx as open
 * errors, which are mapped back to a status here.
 *
 * Linked only into the app: evo_net.c reaches it through a weak symbol, so the
 * host test builds (no FFmpeg) fall back to evo_net's own HTTP code.
 */
#include "evo_net.h"
#include "evo_provider_log.h"

#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "v0.13.5"
#endif

#define AVIO_TIMEOUT_S   15
#define AVIO_MAX_BODY    (16u << 20)   /* a wrong address that serves something huge */

static int status_of_error(int rc)
{
    switch (rc) {
    case AVERROR_HTTP_BAD_REQUEST:  return 400;
    case AVERROR_HTTP_UNAUTHORIZED: return 401;
    case AVERROR_HTTP_FORBIDDEN:    return 403;
    case AVERROR_HTTP_NOT_FOUND:    return 404;
    case AVERROR_HTTP_OTHER_4XX:    return 499;
    case AVERROR_HTTP_SERVER_ERROR: return 500;
    default:                        return 0;
    }
}

/* FFmpeg's binary post_data option takes hex when it is set through a dictionary. */
static char *hex_of(const char *s)
{
    static const char digits[] = "0123456789abcdef";
    size_t n = strlen(s);
    char *out = (char *)malloc(n * 2 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        out[i * 2]     = digits[c >> 4];
        out[i * 2 + 1] = digits[c & 15];
    }
    out[n * 2] = '\0';
    return out;
}

int evo_avio_execute(const char *method, const char *url, const char *post_data,
                     const char **headers, int header_count,
                     char **out_body, size_t *out_len, int *out_status)
{
    if (out_body) *out_body = NULL;
    if (out_len) *out_len = 0;
    if (out_status) *out_status = 0;

    AVDictionary *opts = NULL;
    av_dict_set_int(&opts, "timeout", (int64_t)AVIO_TIMEOUT_S * 1000000, 0);
    av_dict_set_int(&opts, "rw_timeout", (int64_t)AVIO_TIMEOUT_S * 1000000, 0);
    av_dict_set(&opts, "method", method ? method : "GET", 0);
    av_dict_set(&opts, "reconnect", "0", 0);

    size_t cap = 256;
    for (int i = 0; i < header_count; ++i)
        if (headers && headers[i]) cap += strlen(headers[i]) + 2;
    char *head = (char *)malloc(cap);
    if (!head) { av_dict_free(&opts); return EVO_NET_ERR_MEM; }
    head[0] = '\0';
    for (int i = 0; i < header_count; ++i) {
        if (!headers || !headers[i]) continue;
        strcat(head, headers[i]);
        strcat(head, "\r\n");
    }
    if (post_data && post_data[0]) {
        char *hex = hex_of(post_data);
        if (!hex) { free(head); av_dict_free(&opts); return EVO_NET_ERR_MEM; }
        av_dict_set(&opts, "post_data", hex, 0);
        free(hex);
        strcat(head, "Content-Type: application/json\r\n");
    }
    if (head[0]) av_dict_set(&opts, "headers", head, 0);
    free(head);

    AVIOContext *io = NULL;
    const int rc = avio_open2(&io, url, AVIO_FLAG_READ, NULL, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        char err[128];
        av_strerror(rc, err, sizeof err);
        const int st = status_of_error(rc);
        PROV_WARN("avio: %s failed: %s (rc=%d http=%d)", method ? method : "GET", err, rc, st);
        if (out_status) *out_status = st;
        return st ? EVO_NET_ERR_HTTP : EVO_NET_ERR_CONNECT;
    }

    size_t used = 0, size = 4096;
    char *buf = (char *)malloc(size);
    if (!buf) { avio_closep(&io); return EVO_NET_ERR_MEM; }
    unsigned char chunk[16384];
    int n;
    while (used < AVIO_MAX_BODY && (n = avio_read(io, chunk, sizeof chunk)) > 0) {
        if (used + (size_t)n + 1 > size) {
            while (used + (size_t)n + 1 > size) size *= 2;
            char *grown = (char *)realloc(buf, size);
            if (!grown) { free(buf); avio_closep(&io); return EVO_NET_ERR_MEM; }
            buf = grown;
        }
        memcpy(buf + used, chunk, (size_t)n);
        used += (size_t)n;
    }
    avio_closep(&io);
    if (used >= AVIO_MAX_BODY) {
        free(buf);
        return EVO_NET_ERR_BODY_LIMIT;
    }
    buf[used] = '\0';
    if (out_body) *out_body = buf; else free(buf);
    if (out_len) *out_len = used;
    if (out_status) *out_status = 200;   /* avio exposes no 2xx detail: a 204 is an empty 200 */
    return EVO_NET_OK;
}

/*
 * A GET whose body goes to sink() piece by piece instead of into one buffer, for a
 * reply too big to hold (the Xtream guide, #159). sink returns non-zero to stop.
 * No size cap: the caller keeps its own memory flat. The read timeout still applies
 * between pieces, so a stalled panel ends the request.
 */
int evo_avio_stream(const char *url, int (*sink)(const char *, size_t, void *),
                    void *ud, int *out_status)
{
    if (out_status) *out_status = 0;

    AVDictionary *opts = NULL;
    av_dict_set_int(&opts, "timeout", (int64_t)AVIO_TIMEOUT_S * 1000000, 0);
    av_dict_set_int(&opts, "rw_timeout", (int64_t)AVIO_TIMEOUT_S * 1000000, 0);
    av_dict_set(&opts, "method", "GET", 0);
    av_dict_set(&opts, "reconnect", "0", 0);
    /* evo_net's own User-Agent: panels refuse FFmpeg's default "Lavf" one (HTTP 4xx). */
    av_dict_set(&opts, "user_agent", "EVOPlayer-PS5/" EVO_PLAYER_VERSION, 0);

    AVIOContext *io = NULL;
    const int rc = avio_open2(&io, url, AVIO_FLAG_READ, NULL, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        char err[128];
        av_strerror(rc, err, sizeof err);
        const int st = status_of_error(rc);
        PROV_ERR("avio: stream failed: %s (rc=%d http=%d)", err, rc, st);
        if (out_status) *out_status = st;
        return st ? EVO_NET_ERR_HTTP : EVO_NET_ERR_CONNECT;
    }

    int result = EVO_NET_OK;
    const size_t chunk_sz = 65536;      /* heap: a worker thread's stack is small */
    unsigned char *chunk = (unsigned char *)malloc(chunk_sz);
    if (!chunk) { avio_closep(&io); return EVO_NET_ERR_MEM; }
    int n;
    while ((n = avio_read(io, chunk, (int)chunk_sz)) > 0) {
        if (sink((const char *)chunk, (size_t)n, ud) != 0) { result = EVO_NET_ERR_MEM; break; }
    }
    free(chunk);
    if (result == EVO_NET_OK && n < 0 && n != AVERROR_EOF) result = EVO_NET_ERR_CONNECT;
    avio_closep(&io);
    if (out_status) *out_status = 200;
    return result;
}
